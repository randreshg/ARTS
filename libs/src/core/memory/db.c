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

#include "arts/memory/db.h"

#include <assert.h>
#include <pthread.h>
#include <string.h>

#include <hwloc.h>

#include "arts.h"
#include "arts/compute/edt.h"
#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/gas/guid.h"
#include "arts/gas/out_of_order.h"
#include "arts/gas/route_table.h"
#include "arts/memory/frontier.h"
#include "arts/remote/handler.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/sync/termination.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#endif

ARTS_TYPE_NAME;
ARTS_DB_TYPE_NAME;
DB_MODE_NAME;

static bool arts_is_write_mode(arts_db_access_mode_t mode) {
  return mode == DB_MODE_EW || mode == DB_MODE_MEMSET;
}

static bool arts_seen_write_dep(arts_edt_dep_t *depv, int end,
                                arts_guid_t guid, void *ptr) {
  if (guid == NULL_GUID || !ptr) {
    return false;
  }
  for (int j = 0; j < end; j++) {
    if (depv[j].guid == guid && depv[j].ptr == ptr &&
        arts_is_write_mode(depv[j].mode)) {
      return true;
    }
  }
  return false;
}

extern ARTS_THREAD_LOCAL struct arts_edt_s *current_edt;
extern unsigned int num_numa_domains;

static pthread_once_t arts_db_numa_once = PTHREAD_ONCE_INIT;
static hwloc_topology_t arts_db_numa_topology = NULL;
static hwloc_bitmap_t arts_db_numa_nodeset = NULL;
static bool arts_db_numa_interleave_supported = false;

// True for DB subtypes that have a CDAG frontier (remote-capable).
static inline bool arts_db_subtype_has_frontier(arts_db_types_t db_type) {
  return db_type != ARTS_DB_LOCAL;
}

#define WRITE_SET 0x80000000
#define ARTS_DB_NUMA_ALIGN 4096U

static void arts_db_numa_init_once(void) {
  hwloc_topology_t topology = NULL;
  if (hwloc_topology_init(&topology) < 0)
    return;
  if (hwloc_topology_load(topology) < 0) {
    hwloc_topology_destroy(topology);
    return;
  }

  hwloc_bitmap_t nodeset = NULL;
  hwloc_obj_t root = hwloc_get_root_obj(topology);
  if (root && root->nodeset && !hwloc_bitmap_iszero(root->nodeset)) {
    nodeset = hwloc_bitmap_dup(root->nodeset);
  } else {
    nodeset = hwloc_bitmap_alloc();
    hwloc_obj_t numa = NULL;
    while ((numa = hwloc_get_next_obj_by_type(topology, HWLOC_OBJ_NUMANODE,
                                              numa))) {
      hwloc_bitmap_set(nodeset, numa->os_index);
    }
  }

  if (!nodeset || hwloc_bitmap_weight(nodeset) <= 1) {
    if (nodeset)
      hwloc_bitmap_free(nodeset);
    hwloc_topology_destroy(topology);
    return;
  }

  arts_db_numa_topology = topology;
  arts_db_numa_nodeset = nodeset;
  arts_db_numa_interleave_supported = true;
}

static bool arts_db_apply_numa_interleave(void *ptr, size_t size) {
  if (!ptr || size == 0 || num_numa_domains <= 1)
    return false;

  pthread_once(&arts_db_numa_once, arts_db_numa_init_once);
  if (!arts_db_numa_interleave_supported || !arts_db_numa_topology ||
      !arts_db_numa_nodeset)
    return false;

  int flags = HWLOC_MEMBIND_BYNODESET | HWLOC_MEMBIND_NOCPUBIND;
  if (hwloc_set_area_membind(arts_db_numa_topology, ptr, size,
                             arts_db_numa_nodeset, HWLOC_MEMBIND_INTERLEAVE,
                             flags) == 0)
    return true;

  flags = HWLOC_MEMBIND_BYNODESET;
  return hwloc_set_area_membind(arts_db_numa_topology, ptr, size,
                                arts_db_numa_nodeset, HWLOC_MEMBIND_INTERLEAVE,
                                flags) == 0;
}

/*
 * arts_db_auto_acquire — Automatically acquire WRITE access for the creator
 * EDT.
 *
 * Called when an EDT creates a local DB.  Sets WRITE_SET on the initial
 * frontier (blocking all consumers from joining the HEAD frontier).
 * The DB's GUID is tracked in the thread-local created_db_list for cleanup
 * when the EDT completes.
 */
static void arts_db_auto_acquire(struct arts_db_s *db) {
  if (db->db_list) {
    struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
    arts_atomic_fetch_or(&db_list->head->lock, WRITE_SET);
  }
  arts_track_created_db(db->guid);
}

void *arts_db_malloc(arts_db_types_t db_type, size_t size) {
  (void)db_type;
  void *ptr = NULL;
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu) {
    if (db_type == ARTS_DB_LC)
      ptr = arts_cuda_malloc_host(size * 2);
    else if (db_type == ARTS_DB_GPU)
      ptr = arts_cuda_malloc_host(size);
  }
#endif
  if (!ptr) {
    ptr = arts_malloc_align(size, 16);
  }
  return ptr;
}

static void *arts_db_malloc_interleaved(arts_db_types_t db_type, size_t size) {
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu)
    return arts_db_malloc(db_type, size);
#endif
  void *ptr = arts_malloc_align(size, ARTS_DB_NUMA_ALIGN);
  if (ptr)
    arts_db_apply_numa_interleave(ptr, size);
  return ptr;
}

void arts_db_free(void *ptr) {
  struct arts_db_s *db = (struct arts_db_s *)ptr;
  if (db->db_list && db->db_list != (void *)1) {
    arts_delete_db_list((struct arts_db_list_s *)db->db_list);
    db->db_list = NULL;
  }
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu &&
      (db->db_type == ARTS_DB_GPU || db->db_type == ARTS_DB_LC)) {
    arts_cuda_free_host(ptr);
    ptr = NULL;
  }
#endif
  if (ptr) {
    arts_free(ptr);
  }
}

/*
 * arts_db_create_internal — Initialize a DB header in pre-allocated memory.
 *
 * Sets up the arts_db_s header fields (type, size, version, reader/writer
 * counts, db_list) and records metrics.  The caller is responsible for
 * route-table registration.
 */
void arts_db_create_internal(arts_guid_t guid, void *addr, uint64_t len,
                             uint64_t packet_size, arts_db_types_t db_type,
                             uint64_t arts_id) {
  (void)len;
  struct arts_header_s *header = (struct arts_header_s *)addr;
  header->type = ARTS_DB; // All DB subtypes share one GUID type tag
  header->size = packet_size;

  struct arts_db_s *db_res = (struct arts_db_s *)header;
  db_res->arts_id = arts_id;
  db_res->guid = guid;
  db_res->version = 0;
  db_res->reader = 0;
  db_res->writer = 0;
  db_res->copy_count = 1;
  db_res->db_type = db_type;
  if (db_type != ARTS_DB_LOCAL) {
    db_res->db_list = arts_new_db_list();
  } else {
    db_res->db_list = NULL;
  }
  if (db_type == ARTS_DB_LC) {
    void *shadow_copy = (void *)(((char *)addr) + packet_size);
    memcpy(shadow_copy, addr, sizeof(struct arts_db_s));
  }
  // Record per-object DB metrics
  arts_object_record_db(arts_id, packet_size, 0, 0);
  arts_object_trace_db(arts_id, packet_size, 0);
  INCREMENT_NUM_DB_CREATE_BY(1);
  INCREMENT_BYTES_DB_CREATE_BY(len);
}

/*
 * arts_db_create — Unified DataBlock creation.
 *
 * Handles all DB subtypes (DEFAULT, LOCAL, GPU, LC).  When hint->route
 * targets a remote node, sends a stub via ARTS_REMOTE_DB_SEND_MSG and
 * sets *addr = NULL.
 */
arts_guid_t arts_db_create(void **addr, uint64_t len, arts_db_types_t db_type,
                           const arts_hint_t *hint) {
  TIME_DB_CREATE_START();
  unsigned int route = (hint && hint->route != ARTS_HINT_CURRENT_NODE)
                           ? hint->route
                           : arts_global_rank_id;
  uint64_t arts_id = hint ? hint->id : 0;
  arts_guid_t guid = NULL_GUID;

  if (route == arts_global_rank_id) {
    uint64_t db_size = len + sizeof(struct arts_db_s);
    void *ptr = arts_db_malloc(db_type, db_size);
    if (ptr) {
      guid = arts_guid_create_for_rank(arts_global_rank_id, ARTS_DB);
      arts_db_create_internal(guid, ptr, len, db_size, db_type, arts_id);
      arts_route_table_add_item(ptr, guid, arts_global_rank_id, false);
      if (current_edt) {
        arts_db_auto_acquire((struct arts_db_s *)ptr);
      }
      *addr = (void *)((struct arts_db_s *)ptr + 1);
      ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Id:%lu, Type:%s, Size:%lu] "
                 "created locally",
                 guid, arts_id, GET_DB_TYPE_NAME(db_type), len);
    }
  } else {
    guid = arts_guid_create_for_rank(route, ARTS_DB);
    void *ptr = arts_db_malloc(db_type, sizeof(struct arts_db_s));
    struct arts_db_s *db = (struct arts_db_s *)ptr;
    db->header.type = ARTS_DB;
    db->header.size = len + sizeof(struct arts_db_s);
    db->guid = guid;
    db->db_type = db_type;
    db->db_list = (void *)1;
    // Send stub using arts_remote_db_send_packet_s format (matches receiver).
    // Only the header struct is sent; the receiver allocates the full size.
    struct arts_remote_db_send_packet_s send_pkt;
    uint64_t pkt_size = sizeof(send_pkt) + sizeof(struct arts_db_s);
    arts_fill_packet_header(&send_pkt.header, pkt_size,
                            ARTS_REMOTE_DB_SEND_MSG);
    arts_remote_send_request_payload_async_free(
        (int)route, (char *)&send_pkt, sizeof(send_pkt), (char *)ptr, 0,
        sizeof(struct arts_db_s), arts_db_free);
    arts_route_table_remove_item(guid);
    *addr = NULL;
    ARTS_DEBUG("arts_db_create: DB[Guid:%lu, Id:%lu, Type:%s, Size:%lu] "
               "created remotely on rank %u",
               guid, arts_id, GET_DB_TYPE_NAME(db_type), len, route);
  }
  TIME_DB_CREATE_STOP();
  return guid;
}

// Guid must be for a local DB only
static void *arts_db_create_with_guid_impl(arts_guid_t guid, uint64_t len,
                                           arts_db_types_t db_type,
                                           const void *data,
                                           const arts_hint_t *hint,
                                           bool interleave_memory) {
  TIME_DB_CREATE_START();
  uint64_t arts_id = hint ? hint->id : 0;

  void *ptr = NULL;
  if (arts_guid_is_local(guid)) {
    uint64_t db_size = len + sizeof(struct arts_db_s);

    if (interleave_memory) {
      ptr = arts_db_malloc_interleaved(db_type, db_size);
    } else {
      ptr = arts_db_malloc(db_type, db_size);
    }
    if (ptr) {
      struct arts_db_s *db_header = (struct arts_db_s *)ptr;
      arts_db_create_internal(guid, db_header, len, db_size, db_type, arts_id);
      if (data) {
        memcpy((void *)(db_header + 1), data, len);
      }
      if (arts_route_table_add_item_race(db_header, guid, arts_global_rank_id,
                                         false)) {
        arts_route_table_fire_oo(guid, arts_out_of_order_handler);
      }
      if (current_edt) {
        arts_db_auto_acquire(db_header);
      }
      ptr = (void *)(db_header + 1);
    }
  }
  ARTS_INFO("Creating DB[Id:%lu, Guid:%lu, Type:%s, Ptr:%p, Route:%d, "
            "Size:%lu]",
            arts_id, guid, GET_DB_TYPE_NAME(db_type), ptr,
            arts_guid_get_rank(guid), len);
  TIME_DB_CREATE_STOP();
  return ptr;
}

void *arts_db_create_with_guid(arts_guid_t guid, uint64_t len,
                               arts_db_types_t db_type, const void *data,
                               const arts_hint_t *hint) {
  return arts_db_create_with_guid_impl(guid, len, db_type, data, hint, false);
}

void *arts_db_create_with_guid_interleaved(arts_guid_t guid, uint64_t len,
                                           arts_db_types_t db_type,
                                           const void *data,
                                           const arts_hint_t *hint) {
  return arts_db_create_with_guid_impl(guid, len, db_type, data, hint, true);
}

void *arts_db_adopt(arts_guid_t guid, struct arts_db_s *db) {
  if (arts_route_table_add_item_race(db, guid, arts_global_rank_id, false)) {
    arts_route_table_fire_oo(guid, arts_out_of_order_handler);
  }
  if (current_edt) {
    arts_db_auto_acquire(db);
  }
  return (void *)(db + 1);
}

void *arts_db_resize_ptr(struct arts_db_s *db_res, unsigned int size,
                         bool copy) {
  if (db_res) {
    unsigned int old_size = db_res->header.size;
    unsigned int new_size = size + sizeof(struct arts_db_s);
    struct arts_db_s *ptr =
        (struct arts_db_s *)arts_calloc_align(1, new_size, 16);
    if (ptr) {
      if (copy) {
        memcpy(ptr, db_res, old_size);
      } else {
        memcpy(ptr, db_res, sizeof(struct arts_db_s));
      }
      arts_free(db_res);
      ptr->header.size = size + sizeof(struct arts_db_s);
      return (void *)(ptr + 1);
    }
  }
  return NULL;
}

// Must be in write mode (or only copy) to update and alloced (no NO_ACQUIRE
// nonsense), otherwise will be racy...
void *arts_db_resize(arts_guid_t guid, unsigned int size, bool copy) {
  struct arts_db_s *db_res =
      (struct arts_db_s *)arts_route_table_lookup_db(guid, NULL, false);
  void *ptr = arts_db_resize_ptr(db_res, size, copy);
  if (ptr) {
    db_res = ((struct arts_db_s *)ptr) - 1;
  }
  if (db_res) {
    arts_route_table_return_db(guid, false);
  }
  return ptr;
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
  arts_type_t type = arts_guid_get_type(guid);
  if (type != ARTS_DB) {
    ARTS_WARN("arts_db_destroy called with non-DB type %u (GUID %lu)", type,
              guid);
    return;
  }

  /* Implicit release: release the caller's acquire if held. */
  arts_db_release(guid);

  struct arts_db_s *db_res =
      (struct arts_db_s *)arts_route_table_lookup_db(guid, NULL, false);
  if (db_res != NULL) {
    if (arts_db_subtype_has_frontier(db_res->db_type)) {
      arts_remote_db_destroy(guid, arts_global_rank_id);
    }
    arts_route_table_return_db(guid, false);
    arts_route_table_mark_delete(guid);
  } else {
    arts_remote_db_destroy(guid, arts_global_rank_id);
  }
}

bool arts_db_rename_with_guid(arts_guid_t new_guid, arts_guid_t old_guid) {
  bool ret = false;
  unsigned int rank = arts_guid_get_rank(old_guid);
  if (rank == arts_global_rank_id) {
    struct arts_db_s *db_res =
        (struct arts_db_s *)arts_route_table_lookup_db(old_guid, NULL, false);
    if (db_res != NULL) {
      db_res->guid = new_guid;
      // This is only being done by the owner...
      arts_route_table_hide_item(old_guid);
      if (arts_route_table_add_item_race(db_res, new_guid, arts_global_rank_id,
                                         false)) {
        arts_route_table_fire_oo(new_guid, arts_out_of_order_handler);
      }
      arts_route_table_return_db(old_guid, false);
      ret = true;
    }
  } else {
    arts_remote_db_rename(new_guid, old_guid);
  }
  return ret;
}

arts_guid_t arts_db_copy_to_new_type(arts_guid_t old_guid,
                                     arts_db_types_t new_type) {
  arts_guid_t ret = NULL_GUID;
  unsigned int rank = arts_guid_get_rank(old_guid);
  if (rank == arts_global_rank_id) {
    arts_guid_t new_guid = arts_guid_create_for_rank(rank, ARTS_DB);
    struct arts_db_s *db_res =
        (struct arts_db_s *)arts_route_table_lookup_db(old_guid, NULL, false);
    if (db_res != NULL) {
      arts_atomic_add(&db_res->copy_count, 1);
      db_res->guid = new_guid;
      db_res->db_type = new_type;
      if (arts_route_table_add_item_race(db_res, new_guid, arts_global_rank_id,
                                         false)) {
        arts_route_table_fire_oo(new_guid, arts_out_of_order_handler);
      }
      arts_route_table_return_db(old_guid, false);
      ret = new_guid;
    }
  }
  return ret;
}

arts_guid_t arts_db_rename(arts_guid_t guid) {
  arts_guid_t new_guid = arts_guid_create_for_rank(arts_guid_get_rank(guid),
                                                   arts_guid_get_type(guid));
  return (arts_db_rename_with_guid(new_guid, guid)) ? new_guid : NULL_GUID;
}

void arts_db_destroy_safe(arts_guid_t guid, bool remote) {
  /* Implicit release: release the caller's acquire if held. */
  arts_db_release(guid);

  struct arts_db_s *db_res =
      (struct arts_db_s *)arts_route_table_lookup_db(guid, NULL, false);
  if (db_res != NULL) {
    if (remote && arts_db_subtype_has_frontier(db_res->db_type)) {
      arts_remote_db_destroy(guid, arts_global_rank_id);
    }
    arts_route_table_return_db(guid, false);
    arts_route_table_mark_delete(guid);
  } else if (remote) {
    // No local copy — forward destroy to remote if this is a DB GUID
    arts_remote_db_destroy(guid, arts_global_rank_id);
  }
}

/**********************DB MEMORY MODEL*************************************/
// Side Effects: edt depc_needed will be incremented, ptr will be updated,
//   and launches out of order handleReadyEdt
// Returns false on out of order and true otherwise
void acquire_dbs(struct arts_edt_s *edt) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  edt->depc_needed = edt->depc + 1;
  ARTS_INFO("Acquiring %u DBs for EDT[Id:%lu, Guid:%lu], depc_needed "
            "initialized to %u",
            edt->depc, edt->arts_id, edt->current_edt, edt->depc_needed);

  /* Build GUID-sorted index array for deadlock-free acquisition order.
   * Acquiring DBs in ascending GUID order prevents circular wait when
   * multiple EDTs need overlapping DB sets in EW mode. */
  uint32_t sorted[edt->depc > 0 ? edt->depc : 1];
  for (uint32_t k = 0; k < edt->depc; k++) {
    sorted[k] = k;
  }
  /* Insertion sort by GUID — stable, handles duplicates, fast for small N. */
  for (uint32_t k = 1; k < edt->depc; k++) {
    uint32_t val = sorted[k];
    int j = (int)k - 1;
    while (j >= 0 && depv[sorted[j]].guid > depv[val].guid) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = val;
  }

  for (uint32_t si = 0; si < edt->depc; si++) {
    int i = (int)sorted[si]; /* Acquire in GUID order, not slot order. */
    /*
     * A slot with guid == NULL_GUID (0) but mode != DB_MODE_NULL was
     * signaled via an event that carried no data — already satisfied,
     * no DB to acquire.  Count it immediately.
     */
    if (depv[i].guid == NULL_GUID && depv[i].mode != DB_MODE_NULL) {
      arts_atomic_sub(&edt->depc_needed, 1U);
      continue;
    }
    if (depv[i].guid && depv[i].ptr == NULL) {
      arts_db_access_mode_t access_mode = depv[i].mode;
      if (access_mode == DB_MODE_RW && arts_global_rank_count > 1) {
        ARTS_DEBUG("Promoting multinode DB_MODE_RW acquire to DB_MODE_EW "
                   "[Guid:%lu, EdtGuid:%lu, Slot:%u]",
                   depv[i].guid, edt->current_edt, i);
        access_mode = DB_MODE_EW;
        depv[i].mode = DB_MODE_EW;
      }
      uint64_t slice_offset = depv[i].slice_offset;
      uint64_t slice_size = depv[i].slice_size;
      bool has_slice = (slice_size != 0);
      bool prefer_duplicate =
          (depv[i].flags & ARTS_DEP_FLAG_PREFER_DUPLICATE) != 0 &&
          access_mode == DB_MODE_RO;

      /*
       * Value signals (DB_MODE_VALUE) store a raw uint64 in
       * depv[slot].guid — it is NOT a real GUID.  Skip DB acquisition
       * entirely; just count this slot as satisfied.
       */
      if (access_mode == DB_MODE_VALUE) {
        arts_atomic_sub(&edt->depc_needed, 1U);
        continue;
      }

      struct arts_db_s *db_found = NULL;
      int owner = (int)arts_guid_get_rank(depv[i].guid);
      arts_type_t guid_type = arts_guid_get_type(depv[i].guid);

      // Update access-mode counters
      if (access_mode == DB_MODE_RO) {
        INCREMENT_NUM_DB_ACQUIRE_READ_BY(1);
      } else if (access_mode == DB_MODE_EW) {
        INCREMENT_NUM_DB_ACQUIRE_WRITE_BY(1);
        if (owner == arts_global_rank_id) {
          INCREMENT_NUM_OWNER_UPDATE_PERFORMED_BY(1);
        }
      }

      ARTS_INFO("Acquiring DB[Guid:%lu, GuidType:%u, AccessMode:%u, Owner:%d, "
                "Rank:%u] in EDT[Id:%lu, Guid:%lu, Slot:%u] flags=%u",
                depv[i].guid, guid_type, access_mode, owner,
                arts_global_rank_id, edt->arts_id, edt->current_edt, i,
                depv[i].flags);
      ARTS_TRACE_RDMA("db_acquire begin rank=%u edt=%lu slot=%u db=%lu "
                      "mode=%u owner=%d flags=%u",
                      arts_global_rank_id, edt->current_edt, i, depv[i].guid,
                      access_mode, owner, depv[i].flags);

      if (guid_type == ARTS_DB) {
        // Look up DB first — subtype dispatch requires the struct
        int valid_rank = -1;
        struct arts_db_s *db_temp =
            (struct arts_db_s *)arts_route_table_lookup_db(depv[i].guid,
                                                           &valid_rank, true);
        /* Track whether lookup acquired a route table ref so we can
         * return it if the dep is deferred (frontier/remote/OO). */
        bool lookup_ref_held = (db_temp != NULL);

        if (has_slice) {
          if (access_mode != DB_MODE_RO) {
            ARTS_ERROR("Deferred DB slice acquire only supports DB_MODE_RO "
                       "[Guid:%lu, Mode:%s]",
                       depv[i].guid, GET_DB_MODE_NAME(access_mode));
          }

          if (db_temp && db_temp->db_type == ARTS_DB_LOCAL) {
            if (!arts_request_db_slice(db_temp, edt, edt->current_edt, i,
                                       slice_offset, slice_size,
                                       depv[i].flags)) {
              ARTS_ERROR("Failed to satisfy local DB slice request "
                         "[Guid:%lu, Slot:%u]",
                         depv[i].guid, i);
            }
          } else if (db_temp && owner == arts_global_rank_id) {
            if (valid_rank == arts_global_rank_id) {
              if (!arts_request_db_slice(db_temp, edt, edt->current_edt, i,
                                         slice_offset, slice_size,
                                         depv[i].flags)) {
                ARTS_ERROR("Failed to queue local DB slice request "
                           "[Guid:%lu, Slot:%u]",
                           depv[i].guid, i);
              }
            } else {
              arts_remote_get_from_db(edt->current_edt, depv[i].guid, i,
                                      (unsigned int)slice_offset,
                                      (unsigned int)slice_size, depv[i].flags,
                                      valid_rank);
            }
          } else if (db_temp) {
            bool local_valid = (valid_rank == arts_global_rank_id);
            if (local_valid) {
              if (!arts_request_db_slice(db_temp, edt, edt->current_edt, i,
                                         slice_offset, slice_size,
                                         depv[i].flags)) {
                ARTS_ERROR("Failed to satisfy cached DB slice request "
                           "[Guid:%lu, Slot:%u]",
                           depv[i].guid, i);
              }
            } else {
              arts_remote_get_from_db(edt->current_edt, depv[i].guid, i,
                                      (unsigned int)slice_offset,
                                      (unsigned int)slice_size, depv[i].flags,
                                      valid_rank);
            }
          } else {
            if (arts_guid_is_local(depv[i].guid)) {
              arts_out_of_order_get_from_db(
                  edt->current_edt, depv[i].guid, i,
                  (unsigned int)slice_offset, (unsigned int)slice_size,
                  depv[i].flags);
            } else {
              arts_remote_get_from_db(edt->current_edt, depv[i].guid, i,
                                      (unsigned int)slice_offset,
                                      (unsigned int)slice_size, depv[i].flags,
                                      owner);
            }
          }

          if (lookup_ref_held) {
            arts_route_table_return_db(depv[i].guid, false);
          }
          continue;
        }

        if (db_temp && db_temp->db_type == ARTS_DB_LOCAL) {
          // LOCAL: direct access, no frontier
          db_found = db_temp;
          ARTS_TRACE_RDMA("db_acquire local rank=%u edt=%lu slot=%u db=%lu "
                          "mode=%u",
                          arts_global_rank_id, edt->current_edt, i,
                          depv[i].guid, access_mode);
          arts_atomic_sub(&edt->depc_needed, 1U);
        } else if (db_temp && access_mode == DB_MODE_LC_SYNC &&
                   owner == arts_global_rank_id) {
          // LC_SYNC on owner — direct lookup, skip frontier
          ARTS_DEBUG("LC_SYNC -> %p", db_temp);
          db_found = db_temp;
          arts_atomic_sub(&edt->depc_needed, 1U);
        } else if (db_temp && owner == arts_global_rank_id) {
          // Owner path: CDAG frontier for DEFAULT/GPU/LC
          bool on_head = false;
          bool duplicate_added =
              arts_add_db_duplicate_ex(db_temp, arts_global_rank_id, edt,
                                       edt->current_edt, i, access_mode,
                                       depv[i].flags, &on_head);
          if (duplicate_added) {
            ARTS_DEBUG("Adding duplicate DB[Guid:%lu] on_head=%d", depv[i].guid,
                       on_head);
            if (prefer_duplicate) {
              ARTS_DEBUG("Compiler requested duplicate-friendly RO acquire for "
                         "DB[Guid:%lu]",
                         depv[i].guid);
            }
          } else {
            ARTS_DEBUG(
                "Duplicate not added DB[Guid:%lu] (rank already tracked)",
                depv[i].guid);
          }

          if (valid_rank == arts_global_rank_id && on_head) {
            db_found = db_temp;
            ARTS_TRACE_RDMA("db_acquire owner-head rank=%u edt=%lu slot=%u "
                            "db=%lu mode=%u",
                            arts_global_rank_id, edt->current_edt, i,
                            depv[i].guid, access_mode);
            arts_atomic_sub(&edt->depc_needed, 1U);
          } else if (valid_rank == arts_global_rank_id) {
            ARTS_DEBUG("EDT[Guid:%lu] deferred to frontier for "
                       "DB[Guid:%lu] (non-head local frontier, unique=%d)",
                       edt->current_edt, depv[i].guid, duplicate_added);
            ARTS_TRACE_RDMA("db_acquire owner-defer rank=%u edt=%lu slot=%u "
                            "db=%lu mode=%u unique=%u",
                            arts_global_rank_id, edt->current_edt, i,
                            depv[i].guid, access_mode,
                            duplicate_added ? 1U : 0U);
          } else {
            if (access_mode == DB_MODE_RO || db_temp->db_type == ARTS_DB_GPU ||
                db_temp->db_type == ARTS_DB_LC) {
              ARTS_TRACE_RDMA("db_acquire cached-remote-request rank=%u "
                              "edt=%lu slot=%u db=%lu mode=%u valid_rank=%d",
                              arts_global_rank_id, edt->current_edt, i,
                              depv[i].guid, access_mode, valid_rank);
              arts_remote_db_request(depv[i].guid, valid_rank, edt, i,
                                     access_mode, depv[i].flags, true);
            } else {
              ARTS_TRACE_RDMA("db_acquire cached-full-request rank=%u edt=%lu "
                              "slot=%u db=%lu mode=%u valid_rank=%d",
                              arts_global_rank_id, edt->current_edt, i,
                              depv[i].guid, access_mode, valid_rank);
              arts_remote_db_full_request(depv[i].guid, valid_rank,
                                          edt->current_edt, i, access_mode);
            }
          }
        } else if (db_temp) {
          // Non-owner path: cached copy management
          bool local_valid = (valid_rank == arts_global_rank_id);
          if (local_valid) {
            db_found = db_temp;
            ARTS_TRACE_RDMA("db_acquire cache-hit rank=%u edt=%lu slot=%u "
                            "db=%lu mode=%u",
                            arts_global_rank_id, edt->current_edt, i,
                            depv[i].guid, access_mode);
            arts_atomic_sub(&edt->depc_needed, 1U);
          } else if (access_mode == DB_MODE_EW) {
            ARTS_TRACE_RDMA("db_acquire full-request rank=%u edt=%lu slot=%u "
                            "db=%lu owner=%u mode=%u",
                            arts_global_rank_id, edt->current_edt, i,
                            depv[i].guid, owner, access_mode);
            arts_remote_db_full_request(depv[i].guid, owner, edt->current_edt,
                                        i, access_mode);
          } else {
            ARTS_TRACE_RDMA("db_acquire remote-request rank=%u edt=%lu slot=%u "
                            "db=%lu owner=%u mode=%u",
                            arts_global_rank_id, edt->current_edt, i,
                            depv[i].guid, owner, access_mode);
            arts_remote_db_request(depv[i].guid, owner, edt, i, access_mode,
                                   depv[i].flags, true);
          }
        } else {
          // DB not in route table — out-of-order or remote
          if (arts_guid_is_local(depv[i].guid)) {
            ARTS_DEBUG("DB[Guid:%lu] out of order request slot %u",
                       depv[i].guid, i);
            arts_out_of_order_handle_db_request(depv[i].guid, edt, i, true);
          } else {
            // Remote DB not cached locally — request from owner
            if (access_mode == DB_MODE_EW) {
              ARTS_TRACE_RDMA("db_acquire missing-full-request rank=%u "
                              "edt=%lu slot=%u db=%lu owner=%u mode=%u",
                              arts_global_rank_id, edt->current_edt, i,
                              depv[i].guid, owner, access_mode);
              arts_remote_db_full_request(depv[i].guid, owner, edt->current_edt,
                                          i, access_mode);
            } else {
              ARTS_TRACE_RDMA("db_acquire missing-remote-request rank=%u "
                              "edt=%lu slot=%u db=%lu owner=%u mode=%u",
                              arts_global_rank_id, edt->current_edt, i,
                              depv[i].guid, owner, access_mode);
              arts_remote_db_request(depv[i].guid, owner, edt, i, access_mode,
                                     depv[i].flags, true);
            }
          }
        }

        /* If the lookup succeeded but the dep was deferred (frontier,
         * remote request, OO), db_found is NULL and the lookup ref was
         * never transferred to depv[i].ptr.  Return it now. */
        if (lookup_ref_held && !db_found) {
          arts_route_table_return_db(depv[i].guid, false);
        }
      } else if (guid_type == ARTS_NULL) {
        arts_atomic_sub(&edt->depc_needed, 1U);
      }

      if (db_found) {
        depv[i].ptr = db_found + 1;
      }
      ARTS_DEBUG("DB[Guid:%lu, Ptr:%p] acquired", depv[i].guid, depv[i].ptr);
    } else {
      arts_atomic_sub(&edt->depc_needed, 1U);
    }
  }
  ARTS_INFO("EDT[Id:%lu, Guid:%lu] has finished acquiring DBs", edt->arts_id,
            edt->current_edt);
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
    if (access_mode == DB_MODE_PTR || access_mode == DB_MODE_VALUE) {
      continue;
    }
    if (depv[i].guid != NULL_GUID && depv[i].ptr && access_mode == DB_MODE_EW) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
      if (db->db_type != ARTS_DB_LOCAL) {
        arts_remote_update_route_table(depv[i].guid, ARTS_HINT_CURRENT_NODE);
      }
      ARTS_DEBUG("[prep_dbs] DB[Id:%lu, Guid:%lu] ptr=%p, db=%p", db->arts_id,
                 depv[i].guid, depv[i].ptr, db);
    }
#ifdef ARTS_USE_GPU
    if (!gpu && depv[i].ptr && access_mode != DB_MODE_LC_SYNC) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db->db_type == ARTS_DB_LC) {
        arts_reader_lock(&db->reader, &db->writer);
        internal_inc_db_version(&db->version);
      }
    }

    if (!gpu && access_mode == DB_MODE_LC_SYNC && depv[i].ptr) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      ARTS_DEBUG("internalLCSync %lu %p", depv[i].guid, db);
      internal_lc_sync_gpu(depv[i].guid, db);
    }
#endif
  }
}

/*
 * release_dbs — Release DB dependencies after EDT execution completes.
 *
 * For WRITE-mode deps owned locally: progresses the CDAG frontier.
 * For WRITE-mode deps on remote owners: sends the updated data back to
 * the owner node.  For READ-mode: returns the route table entry.
 * For LC DBs (GPU builds): releases the reader lock.
 */
void release_dbs(unsigned int depc, arts_edt_dep_t *depv, bool gpu) {
  for (int i = 0; i < depc; i++) {
    arts_db_access_mode_t access_mode = depv[i].mode;
    if (access_mode == DB_MODE_PTR) {
      ARTS_DEBUG("Releasing DB slice copy [Guid:%lu]", depv[i].guid);
      if (depv[i].ptr) {
        arts_free(depv[i].ptr);
        depv[i].ptr = NULL;
      }
      continue;
    }

    // Get DB subtype from struct when ptr is available.
    // Guard with guid != NULL_GUID: arts_db_release may have nulled the
    // guid while leaving ptr non-NULL (or the DB may have been freed).
    arts_db_types_t db_subtype = ARTS_DB_DEFAULT;
    if (depv[i].guid != NULL_GUID && depv[i].ptr &&
        access_mode != DB_MODE_VALUE) {
      struct arts_db_s *db_hdr = ((struct arts_db_s *)depv[i].ptr) - 1;
      db_subtype = db_hdr->db_type;
    }

    ARTS_DEBUG("Releasing DB[Guid:%lu] [AccessMode:%s, DbSubtype:%s]",
               depv[i].guid, GET_DB_MODE_NAME(access_mode),
               GET_DB_TYPE_NAME(db_subtype));
    unsigned int owner = arts_guid_get_rank(depv[i].guid);

    if (depv[i].guid != NULL_GUID && arts_is_write_mode(access_mode)) {
      if (!arts_seen_write_dep(depv, i, depv[i].guid, depv[i].ptr)) {
        if (db_subtype == ARTS_DB_LOCAL) {
          ARTS_DEBUG("Pinned DB write release (no frontier update)");
        } else if (owner == arts_global_rank_id) {
          struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr - 1);
          if (db->db_list && db->db_list != (void *)1) {
            arts_progress_frontier(db, arts_global_rank_id);
          } else {
            ARTS_DEBUG("DB[Guid:%lu] write release has no frontier",
                       depv[i].guid);
          }
        } else {
          arts_remote_update_db(depv[i].guid, true);
          INCREMENT_NUM_OWNER_UPDATE_PERFORMED_BY(1);
        }
      } else {
        ARTS_DEBUG("DB[Guid:%lu] duplicate WRITE dep release: frontier "
                   "already progressed for this EDT",
                   depv[i].guid);
      }
    } else if (depv[i].guid != NULL_GUID && access_mode == DB_MODE_RO) {
      ARTS_DEBUG("DB[Guid:%lu] released in READ mode (no owner update, no "
                 "latch decrement)",
                 depv[i].guid);
      INCREMENT_NUM_OWNER_UPDATE_SAVED_BY(1);
    } else if (!gpu && db_subtype == ARTS_DB_LC) {
      if (depv[i].ptr) {
        struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
        arts_reader_unlock(&db->reader);
      }
    }

    /* Return the route table ref acquired during DB resolution.
     * PTR mode uses a malloc'd copy (no route table ref), VALUE mode stores
     * a raw uint64 (not a real GUID), NULL_GUID has no entry. */
    if (depv[i].guid != NULL_GUID && access_mode != DB_MODE_PTR &&
        access_mode != DB_MODE_VALUE && depv[i].ptr) {
      arts_route_table_return_db(depv[i].guid, false);
    }
  }
}

/*
 * arts_db_release — Release access to a single DB mid-EDT.
 *
 * Two search paths:
 *   1. created_db_list — DBs the current EDT created (auto-acquired WRITE).
 *   2. depv — DBs received as dependencies (EW or RO mode).
 *
 * For WRITE-mode (EW) deps: progresses the CDAG frontier.  For READ-mode
 * (RO) deps: no frontier action needed (readers don't hold locks).
 *
 * Marks the released slot/entry as NULL_GUID to prevent double-release
 * in the epilogue (release_dbs / arts_release_created_dbs).
 */
void arts_db_release(arts_guid_t guid) {
  /* Path 1: check created_db_list (DBs this EDT created) */
  arts_array_list_t *list = arts_get_created_db_list();
  if (list) {
    uint64_t count = arts_length_array_list(list);
    for (uint64_t i = count; i > 0; i--) {
      arts_guid_t *g = (arts_guid_t *)arts_get_from_array_list(list, i - 1);
      if (*g == guid) {
        *g = NULL_GUID;
        struct arts_db_s *db =
            (struct arts_db_s *)arts_route_table_lookup_db(guid, NULL, false);
        if (db) {
          if (db->db_list) {
            arts_progress_frontier(db, arts_global_rank_id);
          }
          arts_route_table_return_db(guid, false);
        }
        return;
      }
    }
  }

  /* Path 2: check current EDT's depv (dependency-acquired DBs) */
  if (!current_edt) {
    return;
  }
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(current_edt);
  bool released = false;
  bool progressed_write = false;
  for (int i = 0; i < current_edt->depc; i++) {
    if (depv[i].guid != guid) {
      continue;
    }
    arts_db_access_mode_t mode = depv[i].mode;
    if (mode == DB_MODE_PTR) {
      if (depv[i].ptr) {
        arts_free(depv[i].ptr);
      }
      depv[i].guid = NULL_GUID;
      depv[i].ptr = NULL;
      depv[i].mode = DB_MODE_NULL;
      depv[i].slice_offset = 0;
      depv[i].slice_size = 0;
      released = true;
      continue;
    }
    if (arts_is_write_mode(mode) && !progressed_write) {
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db) {
        arts_db_types_t subtype = db->db_type;
        if (subtype == ARTS_DB_LOCAL) {
          /* LOCAL: no frontier, ordering is programmer's responsibility. */
        } else if (arts_guid_get_rank(guid) == arts_global_rank_id) {
          arts_progress_frontier(db, arts_global_rank_id);
        } else {
          arts_remote_update_db(guid, true);
        }
      }
      progressed_write = true;
    }
    /* RO mode: no frontier/latch action needed */
    if (depv[i].ptr) {
      arts_route_table_return_db(guid, false);
    }
    depv[i].guid = NULL_GUID;
    depv[i].ptr = NULL;
    depv[i].mode = DB_MODE_NULL;
    depv[i].slice_offset = 0;
    depv[i].slice_size = 0;
    released = true;
  }
  if (released) {
    return;
  }
}

/*
 * arts_release_created_dbs — Release auto-acquired WRITE access for all DBs
 * created by the current EDT.
 *
 * Mirrors release_dbs() but operates on the thread-local created_db_list
 * instead of the EDT's depv[].  For each tracked DB: progresses the frontier
 * (unblocking consumers).
 * Skips entries already released via arts_db_release() (marked NULL_GUID).
 */
void arts_release_created_dbs(void) {
  arts_array_list_t *list = arts_get_created_db_list();
  if (!list) {
    return;
  }
  uint64_t count = arts_length_array_list(list);
  for (uint64_t i = 0; i < count; i++) {
    arts_guid_t *guid = (arts_guid_t *)arts_get_from_array_list(list, i);
    if (*guid == NULL_GUID) {
      continue;
    }
    struct arts_db_s *db =
        (struct arts_db_s *)arts_route_table_lookup_db(*guid, NULL, false);
    if (db) {
      if (db->db_list) {
        arts_progress_frontier(db, arts_global_rank_id);
      }
      arts_route_table_return_db(*guid, false);
    }
  }
}

bool arts_add_db_duplicate(struct arts_db_s *db, unsigned int rank,
                           struct arts_edt_s *edt, arts_guid_t edt_guid,
                           unsigned int slot, arts_db_access_mode_t mode,
                           bool *on_head) {
  return arts_add_db_duplicate_ex(db, rank, edt, edt_guid, slot, mode, 0,
                                  on_head);
}

bool arts_add_db_duplicate_ex(struct arts_db_s *db, unsigned int rank,
                              struct arts_edt_s *edt, arts_guid_t edt_guid,
                              unsigned int slot, arts_db_access_mode_t mode,
                              uint32_t flags, bool *on_head) {
  if (!db || db->db_type == ARTS_DB_LOCAL || db->db_list == (void *)1) {
    if (on_head)
      *on_head = true;
    return true;
  }
  if (!db->db_list) {
    struct arts_db_list_s *new_list = arts_new_db_list();
    if (arts_atomic_cswap_ptr((volatile void **)&db->db_list, NULL,
                              new_list)) {
      arts_delete_db_list(new_list);
    }
  }

  bool write = (mode == DB_MODE_EW || mode == DB_MODE_MEMSET);
  bool prefer_duplicate =
      mode == DB_MODE_RO && (flags & ARTS_DEP_FLAG_PREFER_DUPLICATE) != 0;
  if (edt && edt_guid == NULL_GUID) {
    edt_guid = edt->current_edt;
  }
  return arts_push_db_to_list((struct arts_db_list_s *)db->db_list, rank, write,
                              arts_guid_get_rank(db->guid) == rank,
                              prefer_duplicate, edt, edt_guid, slot, mode,
                              on_head);
}

static unsigned int arts_db_slice_signal_size(struct arts_db_s *db,
                                              unsigned int slice_size,
                                              uint32_t flags) {
  if ((flags & ARTS_DEP_FLAG_PRESERVE_SHAPE) != 0) {
    uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
    if (data_size > UINT_MAX) {
      ARTS_ERROR("DB[Guid:%lu] payload [%lu] exceeds DB_MODE_PTR transport "
                 "limit",
                 db->guid, (unsigned long)data_size);
    }
    return (unsigned int)data_size;
  }
  return slice_size;
}

static void *arts_alloc_db_slice_payload(struct arts_db_s *db,
                                         unsigned int offset,
                                         unsigned int size,
                                         uint32_t flags) {
  uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
  if ((uint64_t)offset > data_size || (uint64_t)size > data_size - offset) {
    ARTS_ERROR("ESD slice [%u, %u) is out of bounds for DB[Guid:%lu, Size:%lu]",
               offset, offset + size, db->guid, (unsigned long)data_size);
  }

  unsigned int signal_size = arts_db_slice_signal_size(db, size, flags);
  void *payload = (flags & ARTS_DEP_FLAG_PRESERVE_SHAPE)
                      ? arts_calloc(1, signal_size)
                      : arts_malloc(signal_size);
  if (size) {
    char *dst = (char *)payload;
    if ((flags & ARTS_DEP_FLAG_PRESERVE_SHAPE) != 0) {
      dst += offset;
    }
    memcpy(dst, ((char *)(db + 1)) + offset, size);
  }
  return payload;
}

void internal_get_from_db(arts_guid_t edt_guid, arts_guid_t db_guid,
                          unsigned int slot, unsigned int offset,
                          unsigned int size, uint32_t flags,
                          unsigned int rank) {
  if (rank == arts_global_rank_id) {
    int valid_rank = -1;
    struct arts_db_s *db = (struct arts_db_s *)arts_route_table_lookup_db(
        db_guid, &valid_rank, false);
    if (db) {
      ARTS_INFO("Getting DB slice [Guid:%lu, Offset:%u, Size:%u] for EDT[%lu]",
                db_guid, offset, size, edt_guid);

      /*
       * Slice delivery is copy-based. Owner-local DBs must still respect the
       * frontier before copying the bytes; cached remote DB copies and
       * ARTS_DB_LOCAL instances can signal immediately.
       */
      if (db->db_type == ARTS_DB_LOCAL ||
          (valid_rank == arts_global_rank_id && !arts_guid_is_local(db_guid))) {
        if (edt_guid != NULL_GUID) {
          if ((flags & ARTS_DEP_FLAG_PRESERVE_SHAPE) != 0) {
            unsigned int signal_size =
                arts_db_slice_signal_size(db, size, flags);
            void *payload =
                arts_alloc_db_slice_payload(db, offset, size, flags);
            arts_signal_edt_ptr_with_guid(edt_guid, slot, db_guid, payload,
                                          signal_size);
            arts_free(payload);
          } else {
            arts_signal_edt_ptr_with_guid(
                edt_guid, slot, db_guid,
                (void *)(((char *)(db + 1)) + offset), size);
          }
        }
      } else if (valid_rank == arts_global_rank_id) {
        if (edt_guid == NULL_GUID) {
          ARTS_ERROR("DB slice request for DB[Guid:%lu] requires a target EDT",
                     db_guid);
        }
        arts_request_db_slice(db, NULL, edt_guid, slot, offset, size, flags);
      } else {
        arts_route_table_return_db(db_guid, false);
        arts_remote_get_from_db(edt_guid, db_guid, slot, offset, size, flags,
                                valid_rank);
        return;
      }
      arts_route_table_return_db(db_guid, false);
    } else {
      assert(edt_guid != NULL_GUID && "DB not found and no EDT to signal");
      ARTS_INFO("Getting OO-DB slice [Guid:%lu, Offset:%u, Size:%u]", db_guid,
                offset, size);
      arts_out_of_order_get_from_db(edt_guid, db_guid, slot, offset, size,
                                    flags);
    }
  } else {
    ARTS_DEBUG("Sending DB slice [Guid:%lu, Offset:%u, Size:%u] via rank %u",
               db_guid, offset, size, rank);
    assert(edt_guid != NULL_GUID && "DB not found and no EDT to signal");
    arts_remote_get_from_db(edt_guid, db_guid, slot, offset, size, flags,
                            rank);
  }
}

void arts_get_from_db(arts_guid_t edt_guid, arts_guid_t db_guid,
                      unsigned int slot, unsigned int offset,
                      unsigned int len) {
  TIME_DB_GET_START();
  INCREMENT_NUM_DB_GET_BY(1);
  unsigned int rank = arts_guid_get_rank(db_guid);
  internal_get_from_db(edt_guid, db_guid, slot, offset, len, 0, rank);
  TIME_DB_GET_STOP();
}

void arts_get_from_db_at(arts_guid_t edt_guid, arts_guid_t db_guid,
                         unsigned int slot, unsigned int offset,
                         unsigned int len, unsigned int rank) {
  arts_get_from_db_at_ex(edt_guid, db_guid, slot, offset, len, 0, rank);
}

void arts_get_from_db_at_ex(arts_guid_t edt_guid, arts_guid_t db_guid,
                            unsigned int slot, unsigned int offset,
                            unsigned int len, uint32_t flags,
                            unsigned int rank) {
  TIME_DB_GET_START();
  INCREMENT_NUM_DB_GET_BY(1);
  internal_get_from_db(edt_guid, db_guid, slot, offset, len, flags, rank);
  TIME_DB_GET_STOP();
}

void internal_put_in_db(void *ptr, arts_guid_t edt_guid, arts_guid_t db_guid,
                        unsigned int slot, unsigned int offset,
                        unsigned int size, arts_guid_t epoch_guid,
                        unsigned int rank) {
  if (rank == arts_global_rank_id) {
    struct arts_db_s *db =
        (struct arts_db_s *)arts_route_table_lookup_db(db_guid, NULL, false);
    if (db) {
      // Do this so when we increment finished we can check the term status
      increment_queue_epoch(epoch_guid);
      arts_shutdown_epoch_inc_queue();
      void *data = (void *)(((char *)(db + 1)) + offset);
      memcpy(data, ptr, size);
      if (edt_guid != NULL_GUID) {
        arts_signal_edt(edt_guid, slot, db_guid, DB_MODE_EW);
      }
      increment_finished_epoch(epoch_guid);
      arts_shutdown_epoch_inc_finished();
      arts_route_table_return_db(db_guid, false);
    } else {
      void *cpy_ptr = arts_malloc(size);
      memcpy(cpy_ptr, ptr, size);
      arts_out_of_order_put_in_db(cpy_ptr, edt_guid, db_guid, slot, offset,
                                  size, epoch_guid);
    }
  } else {
    void *cpy_ptr = arts_malloc(size);
    memcpy(cpy_ptr, ptr, size);
    arts_remote_put_in_db(cpy_ptr, edt_guid, db_guid, slot, offset, size,
                          epoch_guid, rank);
  }
}

void arts_put_in_db_at(void *ptr, arts_guid_t edt_guid, arts_guid_t db_guid,
                       unsigned int slot, unsigned int offset, unsigned int len,
                       unsigned int rank) {
  TIME_DB_PUT_START();
  INCREMENT_NUM_DB_PUT_BY(1);
  INCREMENT_BYTES_DB_PUT_BY(len);
  arts_guid_t epoch_guid = arts_get_current_epoch_guid();
  ARTS_DEBUG("Epoch [Guid:%lu]", epoch_guid);
  increment_active_epoch(epoch_guid);
  arts_shutdown_epoch_inc_active();
  internal_put_in_db(ptr, edt_guid, db_guid, slot, offset, len, epoch_guid,
                     rank);
  TIME_DB_PUT_STOP();
}

void arts_put_in_db(void *ptr, arts_guid_t edt_guid, arts_guid_t db_guid,
                    unsigned int slot, unsigned int offset, unsigned int len) {
  TIME_DB_PUT_START();
  INCREMENT_NUM_DB_PUT_BY(1);
  INCREMENT_BYTES_DB_PUT_BY(len);
  unsigned int rank = arts_guid_get_rank(db_guid);
  arts_guid_t epoch_guid = arts_get_current_epoch_guid();
  ARTS_DEBUG("Epoch [Guid:%lu]", epoch_guid);
  increment_active_epoch(epoch_guid);
  arts_shutdown_epoch_inc_active();
  internal_put_in_db(ptr, edt_guid, db_guid, slot, offset, len, epoch_guid,
                     rank);
  TIME_DB_PUT_STOP();
}

void arts_put_in_db_epoch(void *ptr, arts_guid_t epoch_guid,
                          arts_guid_t db_guid, unsigned int offset,
                          unsigned int len) {
  TIME_DB_PUT_START();
  INCREMENT_NUM_DB_PUT_BY(1);
  INCREMENT_BYTES_DB_PUT_BY(len);
  unsigned int rank = arts_guid_get_rank(db_guid);
  increment_active_epoch(epoch_guid);
  arts_shutdown_epoch_inc_active();
  internal_put_in_db(ptr, NULL_GUID, db_guid, 0, offset, len, epoch_guid, rank);
  TIME_DB_PUT_STOP();
}

/*
 * arts_wait_release_dbs -- Temporarily release frontier locks for all DBs
 * held by the current EDT, allowing consumer EDTs to proceed while this
 * EDT blocks on arts_wait_on_handle.
 *
 * Handles both:
 *   1. created_db_list (auto-acquired WRITE from arts_db_create)
 *   2. depv (dependency-acquired EW/MEMSET)
 *
 * Only touches frontier locks (arts_progress_frontier). Does NOT return
 * route table entries or null any tracking state -- this is a temporary
 * release, not a final epilogue release.
 */
void arts_wait_release_dbs(void) {
  /* Path 1: created DBs */
  arts_array_list_t *list = arts_get_created_db_list();
  if (list) {
    uint64_t count = arts_length_array_list(list);
    for (uint64_t i = 0; i < count; i++) {
      arts_guid_t *guid = (arts_guid_t *)arts_get_from_array_list(list, i);
      if (*guid == NULL_GUID) {
        continue;
      }
      struct arts_db_s *db =
          (struct arts_db_s *)arts_route_table_lookup_db(*guid, NULL, false);
      if (db && db->db_list) {
        arts_progress_frontier(db, arts_global_rank_id);
      }
    }
  }

  /* Path 2: depv DBs */
  if (current_edt) {
    arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(current_edt);
    for (int i = 0; i < current_edt->depc; i++) {
      if (depv[i].guid == NULL_GUID) {
        continue;
      }
      if (!arts_is_write_mode(depv[i].mode)) {
        continue;
      }
      if (!depv[i].ptr) {
        continue;
      }
      if (arts_seen_write_dep(depv, i, depv[i].guid, depv[i].ptr)) {
        continue;
      }
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db && db->db_list) {
        arts_progress_frontier(db, arts_global_rank_id);
      }
    }
  }
}

/*
 * arts_wait_reacquire_dbs -- Re-acquire frontier locks for all DBs
 * held by the current EDT after arts_wait_on_handle completes.
 *
 * After arts_progress_frontier in the release phase, consumers run and their
 * epilogues also call arts_progress_frontier, consuming the entire frontier
 * chain. By the time the epoch completes, db_list->head is typically NULL.
 * In that case, create a fresh frontier node with WRITE_SET as the new head.
 */
void arts_wait_reacquire_dbs(void) {
  /* Path 1: created DBs */
  arts_array_list_t *list = arts_get_created_db_list();
  if (list) {
    uint64_t count = arts_length_array_list(list);
    for (uint64_t i = 0; i < count; i++) {
      arts_guid_t *guid = (arts_guid_t *)arts_get_from_array_list(list, i);
      if (*guid == NULL_GUID) {
        continue;
      }
      struct arts_db_s *db =
          (struct arts_db_s *)arts_route_table_lookup_db(*guid, NULL, false);
      if (db && db->db_list) {
        struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
        arts_writer_lock(&db_list->reader, &db_list->writer);
        if (!db_list->head) {
          struct arts_db_frontier_s *new_f = arts_new_db_frontier();
          arts_atomic_fetch_or(&new_f->lock, WRITE_SET);
          db_list->head = db_list->tail = new_f;
        } else {
          arts_atomic_fetch_or(&db_list->head->lock, WRITE_SET);
        }
        arts_writer_unlock(&db_list->writer);
      }
    }
  }

  /* Path 2: depv DBs */
  if (current_edt) {
    arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(current_edt);
    for (int i = 0; i < current_edt->depc; i++) {
      if (depv[i].guid == NULL_GUID) {
        continue;
      }
      if (!arts_is_write_mode(depv[i].mode)) {
        continue;
      }
      if (!depv[i].ptr) {
        continue;
      }
      if (arts_seen_write_dep(depv, i, depv[i].guid, depv[i].ptr)) {
        continue;
      }
      struct arts_db_s *db = ((struct arts_db_s *)depv[i].ptr) - 1;
      if (db && db->db_list) {
        struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
        arts_writer_lock(&db_list->reader, &db_list->writer);
        if (!db_list->head) {
          struct arts_db_frontier_s *new_f = arts_new_db_frontier();
          arts_atomic_fetch_or(&new_f->lock, WRITE_SET);
          db_list->head = db_list->tail = new_f;
        } else {
          arts_atomic_fetch_or(&db_list->head->lock, WRITE_SET);
        }
        arts_writer_unlock(&db_list->writer);
      }
    }
  }
}
