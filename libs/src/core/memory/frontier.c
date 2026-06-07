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
#include "arts/memory/frontier.h"
#include "arts/gas/route_table.h"

#include <limits.h>
#include <string.h>

#include "arts/utils/malloc.h"

#include "arts/compute/edt.h"
#include "arts/remote/handler.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/utils/atomics.h"

#define WRITE_SET 0x80000000
#define FRONTIER_POOL_MAX 64U

/* Forward declarations for mutually-referenced frontier helpers. */
void arts_signal_frontier_local(struct arts_db_frontier_s *frontier,
                                struct arts_db_s *db);
void arts_signal_frontier_remote(struct arts_db_frontier_s *frontier,
                                 struct arts_db_s *db, unsigned int get_from);
static void arts_serve_ro_readers(struct arts_db_frontier_s *frontier,
                                  struct arts_db_s *db);
static bool arts_reader_gen_drained(struct arts_db_frontier_s *frontier);
static void arts_queue_frontier_delete(
    struct arts_db_frontier_s **to_delete_head,
    struct arts_db_frontier_s **to_delete_tail,
    struct arts_db_frontier_s *frontier);
static void arts_promote_ready_heads_locked(
    struct arts_db_list_s *db_list, struct arts_db_s *db, unsigned int rank,
    struct arts_db_frontier_s **to_delete_head,
    struct arts_db_frontier_s **to_delete_tail);
void arts_delete_db_frontier(struct arts_db_frontier_s *frontier);

void frontier_lock(volatile unsigned int *lock) {
  unsigned int local;
  unsigned int temp;
  while (1) {
    local = *lock;
    if ((local & 1U) == 0) {
      temp = arts_atomic_cswap(lock, local, local | 1U);
      if (temp == local) {
        return;
      }
    }
  }
}

void frontier_unlock(volatile unsigned int *lock) {
  arts_atomic_fetch_and(lock, WRITE_SET);
}

bool frontier_add_read_lock(volatile unsigned int *lock) {
  unsigned int local;
  unsigned int temp;
  while (1) {
    local = *lock;
    // Reject if a writer owns this frontier (or frontier is sealed)
    if ((local & WRITE_SET) != 0) {
      return false;
    }
    if ((local & 1U) == 0) {
      temp = arts_atomic_cswap(lock, local, local | 1U);
      if (temp == local) {
        return true;
      }
    }
  }
}

// Returns true if there is no write in the frontier, false if there is
bool frontier_add_write_lock(volatile unsigned int *lock) {
  unsigned int local;
  unsigned int temp;
  while (1) {
    local = *lock;
    // Reject if another writer already owns this frontier (or sealed)
    if ((local & WRITE_SET) != 0) {
      return false;
    }
    // Wait for lock to be free
    if ((local & 1U) == 0) {
      temp = arts_atomic_cswap(lock, local, local | WRITE_SET | 1U);
      if (temp == local) {
        return true;
      }
    }
  }
}

struct arts_db_element_s *arts_new_db_element() {
  struct arts_db_element_s *ret = (struct arts_db_element_s *)arts_calloc(
      1, sizeof(struct arts_db_element_s));
  if (!ret) {
    ARTS_ERROR("DB element allocation failed");
  }
  return ret;
}

struct arts_db_frontier_s *arts_new_db_frontier() {
  struct arts_db_frontier_s *ret = (struct arts_db_frontier_s *)arts_calloc(
      1, sizeof(struct arts_db_frontier_s));
  if (!ret) {
    ARTS_ERROR("DB frontier allocation failed");
  }
  return ret;
}

struct arts_db_list_s *arts_new_db_list() {
  struct arts_db_list_s *ret =
      (struct arts_db_list_s *)arts_calloc(1, sizeof(struct arts_db_list_s));
  if (!ret) {
    ARTS_ERROR("DB list allocation failed");
  }
  ret->head = ret->tail = arts_new_db_frontier();
  return ret;
}

static void arts_reset_db_element_chain(struct arts_db_element_s *head) {
  for (struct arts_db_element_s *current = head; current;) {
    struct arts_db_element_s *next = current->next;
    memset(current, 0, sizeof(struct arts_db_element_s));
    current->next = next;
    current = next;
  }
}

static void
arts_reset_local_delayed_chain(struct arts_local_delayed_edt_s *head) {
  for (struct arts_local_delayed_edt_s *current = head; current;) {
    struct arts_local_delayed_edt_s *next = current->next;
    memset(current, 0, sizeof(struct arts_local_delayed_edt_s));
    current->next = next;
    current = next;
  }
}

static void
arts_reset_delayed_slice_chain(struct arts_delayed_slice_request_s *head) {
  for (struct arts_delayed_slice_request_s *current = head; current;) {
    struct arts_delayed_slice_request_s *next = current->next;
    memset(current, 0, sizeof(struct arts_delayed_slice_request_s));
    current->next = next;
    current = next;
  }
}

static void arts_reset_ro_reader_chain(struct arts_ro_reader_s *head) {
  for (struct arts_ro_reader_s *current = head; current;) {
    struct arts_ro_reader_s *next = current->next;
    memset(current, 0, sizeof(struct arts_ro_reader_s));
    current->next = next;
    current = next;
  }
}

static void arts_reset_db_frontier_for_reuse(
    struct arts_db_frontier_s *frontier) {
  if (!frontier) {
    return;
  }

  struct arts_db_element_s *list_next = frontier->list.next;
  struct arts_ro_reader_s *ro_readers_next = frontier->roReaders.next;
  struct arts_ro_reader_s *local_ro_readers_next =
      frontier->localRoReaders.next;
  struct arts_local_delayed_edt_s *local_delayed_next =
      frontier->localDelayed.next;
  struct arts_delayed_slice_request_s *slice_delayed_next =
      frontier->sliceDelayed.next;

  if (frontier->exUpdatePayload) {
    arts_free(frontier->exUpdatePayload);
  }

  /* Retired generations are detached under the db_list writer lock before
   * reuse.  Reset every per-generation cursor, including localRoReadersPos, so
   * recycled nodes preserve the calloc state that the RO->EW guard relies on. */
  memset(frontier, 0, sizeof(struct arts_db_frontier_s));
  frontier->list.next = list_next;
  frontier->roReaders.next = ro_readers_next;
  frontier->localRoReaders.next = local_ro_readers_next;
  frontier->localDelayed.next = local_delayed_next;
  frontier->sliceDelayed.next = slice_delayed_next;

  arts_reset_db_element_chain(&frontier->list);
  arts_reset_ro_reader_chain(&frontier->roReaders);
  arts_reset_ro_reader_chain(&frontier->localRoReaders);
  arts_reset_local_delayed_chain(&frontier->localDelayed);
  arts_reset_delayed_slice_chain(&frontier->sliceDelayed);
}

static struct arts_db_frontier_s *
arts_db_list_take_frontier_locked(struct arts_db_list_s *db_list) {
  if (db_list && db_list->free_frontiers) {
    struct arts_db_frontier_s *frontier = db_list->free_frontiers;
    db_list->free_frontiers = frontier->next;
    db_list->free_frontiers_count--;
    frontier->next = NULL;
    arts_reset_db_frontier_for_reuse(frontier);
    return frontier;
  }
  return arts_new_db_frontier();
}

static void
arts_db_list_recycle_frontier_locked(struct arts_db_list_s *db_list,
                                     struct arts_db_frontier_s *frontier) {
  if (!frontier) {
    return;
  }
  if (!db_list || db_list->free_frontiers_count >= FRONTIER_POOL_MAX) {
    arts_delete_db_frontier(frontier);
    return;
  }
  arts_reset_db_frontier_for_reuse(frontier);
  frontier->next = db_list->free_frontiers;
  db_list->free_frontiers = frontier;
  db_list->free_frontiers_count++;
}

static void arts_db_list_recycle_frontier_queue_locked(
    struct arts_db_list_s *db_list, struct arts_db_frontier_s *frontier) {
  while (frontier) {
    struct arts_db_frontier_s *next = frontier->next;
    frontier->next = NULL;
    arts_db_list_recycle_frontier_locked(db_list, frontier);
    frontier = next;
  }
}

static void arts_db_list_repair_tail_locked(struct arts_db_list_s *db_list) {
  if (!db_list) {
    return;
  }
  if (!db_list->head) {
    db_list->tail = NULL;
    return;
  }
  if (!db_list->tail) {
    db_list->tail = db_list->head;
  }
  while (db_list->tail->next) {
    db_list->tail = db_list->tail->next;
  }
}

static struct arts_db_frontier_s *
arts_db_list_get_tail_locked(struct arts_db_list_s *db_list) {
  if (!db_list) {
    return NULL;
  }
  if (!db_list->head) {
    db_list->tail = NULL;
    return NULL;
  }
  if (!db_list->tail || db_list->tail->next) {
    arts_db_list_repair_tail_locked(db_list);
  }
  return db_list->tail;
}

static struct arts_db_frontier_s *
arts_db_list_append_tail_locked(struct arts_db_list_s *db_list) {
  struct arts_db_frontier_s *tail = arts_db_list_get_tail_locked(db_list);
  struct arts_db_frontier_s *next = arts_db_list_take_frontier_locked(db_list);
  if (!tail) {
    db_list->head = db_list->tail = next;
    return next;
  }
  tail->next = next;
  db_list->tail = next;
  return next;
}

static void
arts_db_list_try_advance_tail(struct arts_db_list_s *db_list,
                              struct arts_db_frontier_s *expected,
                              struct arts_db_frontier_s *next) {
  if (!db_list || !expected || !next) {
    return;
  }
  (void)arts_atomic_cswap_ptr((volatile void **)&db_list->tail, expected, next);
}

static struct arts_db_frontier_s *
arts_db_list_pop_head_locked(struct arts_db_list_s *db_list) {
  if (!db_list || !db_list->head) {
    return NULL;
  }
  struct arts_db_frontier_s *head = db_list->head;
  bool popped_tail = (db_list->tail == head);
  db_list->head = head->next;
  if (!db_list->head) {
    db_list->tail = NULL;
  } else if (popped_tail) {
    db_list->tail = db_list->head;
  }
  head->next = NULL;
  return head;
}

void arts_delete_db_element(struct arts_db_element_s *head) {
  struct arts_db_element_s *trail;
  struct arts_db_element_s *current = head;
  while (current) {
    trail = current;
    current = current->next;
    arts_free(trail);
  }
}

void arts_delete_local_delayed_edt(struct arts_local_delayed_edt_s *head) {
  struct arts_local_delayed_edt_s *trail;
  struct arts_local_delayed_edt_s *current = head;
  while (current) {
    trail = current;
    current = current->next;
    arts_free(trail);
  }
}

void arts_delete_delayed_slice_request(struct arts_delayed_slice_request_s *head) {
  struct arts_delayed_slice_request_s *trail;
  struct arts_delayed_slice_request_s *current = head;
  while (current) {
    trail = current;
    current = current->next;
    arts_free(trail);
  }
}

void arts_delete_ro_reader(struct arts_ro_reader_s *head) {
  struct arts_ro_reader_s *current = head;
  while (current) {
    struct arts_ro_reader_s *trail = current;
    current = current->next;
    arts_free(trail);
  }
}

void arts_delete_db_frontier(struct arts_db_frontier_s *frontier) {
  if (frontier->list.next) {
    arts_delete_db_element(frontier->list.next);
  }
  if (frontier->localDelayed.next) {
    arts_delete_local_delayed_edt(frontier->localDelayed.next);
  }
  if (frontier->sliceDelayed.next) {
    arts_delete_delayed_slice_request(frontier->sliceDelayed.next);
  }
  if (frontier->roReaders.next) {
    arts_delete_ro_reader(frontier->roReaders.next);
  }
  if (frontier->localRoReaders.next) {
    arts_delete_ro_reader(frontier->localRoReaders.next);
  }
  if (frontier->exUpdatePayload) {
    arts_free(frontier->exUpdatePayload);
  }
  arts_free(frontier);
}

void arts_delete_db_list(struct arts_db_list_s *db_list) {
  if (!db_list) {
    return;
  }
  struct arts_db_frontier_s *frontier = db_list->head;
  while (frontier) {
    struct arts_db_frontier_s *next = frontier->next;
    arts_delete_db_frontier(frontier);
    frontier = next;
  }
  frontier = db_list->free_frontiers;
  while (frontier) {
    struct arts_db_frontier_s *next = frontier->next;
    arts_delete_db_frontier(frontier);
    frontier = next;
  }
  arts_free(db_list);
}

static void arts_push_ro_reader(struct arts_ro_reader_s *head,
                                unsigned int position, unsigned int node,
                                arts_guid_t edt_guid, unsigned int slot) {
  if (!head) {
    return;
  }
  unsigned int num_elements = position / DBSPERELEMENT;
  unsigned int element_pos = position % DBSPERELEMENT;
  struct arts_ro_reader_s *current = head;
  for (unsigned int i = 0; i < num_elements; i++) {
    if (!current->next) {
      current->next = (struct arts_ro_reader_s *)arts_calloc(
          1, sizeof(struct arts_ro_reader_s));
      if (!current->next) {
        ARTS_ERROR("DB RO reader allocation failed");
      }
    }
    current = current->next;
  }
  current->node[element_pos] = node;
  current->edt_guid[element_pos] = edt_guid;
  current->slot[element_pos] = slot;
}

static bool arts_ro_reader_chain_contains(struct arts_ro_reader_s *head,
                                          unsigned int count,
                                          unsigned int node,
                                          arts_guid_t edt_guid,
                                          unsigned int slot,
                                          bool match_node,
                                          bool match_edt,
                                          bool match_slot) {
  struct arts_ro_reader_s *cur = head;
  for (unsigned int i = 0; i < count && cur; i++) {
    unsigned int pos = i % DBSPERELEMENT;
    bool node_match = !match_node || cur->node[pos] == node;
    bool edt_match = !match_edt || cur->edt_guid[pos] == edt_guid;
    bool slot_match = !match_slot || cur->slot[pos] == slot;
    if (node_match && edt_match && slot_match) {
      return true;
    }
    if (pos + 1 == DBSPERELEMENT) {
      cur = cur->next;
    }
  }
  return false;
}

static bool arts_frontier_has_remote_ew_writer(
    struct arts_db_frontier_s *frontier, unsigned int rank,
    arts_guid_t edt_guid, unsigned int slot, arts_db_access_mode_t mode) {
  return frontier->exPreRegistered && frontier->exNode == rank &&
         frontier->exEdtGuid == edt_guid && frontier->exSlot == slot &&
         frontier->exMode == mode;
}

static bool arts_db_list_has_remote_ew_writer_locked(
    struct arts_db_list_s *db_list, unsigned int rank, arts_guid_t edt_guid,
    unsigned int slot, arts_db_access_mode_t mode) {
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    bool found =
        arts_frontier_has_remote_ew_writer(frontier, rank, edt_guid, slot, mode);
    frontier_unlock(&frontier->lock);
    if (found) {
      return true;
    }
  }
  return false;
}

static bool arts_db_list_has_local_ew_writer_locked(
    struct arts_db_list_s *db_list, arts_guid_t edt_guid) {
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    bool found = frontier->localWriteEdtGuid == edt_guid;
    frontier_unlock(&frontier->lock);
    if (found) {
      return true;
    }
  }
  return false;
}

static bool arts_db_list_has_local_ro_reader_locked(
    struct arts_db_list_s *db_list, arts_guid_t edt_guid, unsigned int slot) {
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    bool found = arts_ro_reader_chain_contains(
        &frontier->localRoReaders, frontier->localRoReadersPos,
        arts_global_rank_id, edt_guid, slot, true, true, true);
    frontier_unlock(&frontier->lock);
    if (found) {
      return true;
    }
  }
  return false;
}

static bool arts_db_list_has_remote_ro_reader_locked(
    struct arts_db_list_s *db_list, unsigned int rank, arts_guid_t edt_guid,
    unsigned int slot, bool match_edt, bool match_slot) {
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    bool found =
        arts_ro_reader_chain_contains(&frontier->roReaders,
                                      frontier->roReadersCount, rank, edt_guid,
                                      slot, true, match_edt, match_slot);
    frontier_unlock(&frontier->lock);
    if (found) {
      return true;
    }
  }
  return false;
}

bool arts_push_db_to_element(struct arts_db_element_s *head,
                             unsigned int position, unsigned int data) {
  unsigned int j = 0;
  for (struct arts_db_element_s *current = head; current;
       current = current->next) {
    for (unsigned int i = 0; i < DBSPERELEMENT; i++) {
      if (j < position) {
        if (current->array[i] == data) {
          return false;
        }
        j++;
      } else {
        current->array[i] = data;
        return true;
      }
    }
    if (!current->next) {
      current->next = arts_new_db_element();
    }
  }
  // Need to mark unreachable
  return false;
}

void arts_push_delayed_edt(struct arts_local_delayed_edt_s *head,
                           unsigned int position, struct arts_edt_s *edt,
                           unsigned int slot, arts_db_access_mode_t mode) {
  if (!head) {
    return;
  }
  unsigned int num_elements = position / DBSPERELEMENT;
  unsigned int element_pos = position % DBSPERELEMENT;
  struct arts_local_delayed_edt_s *current = head;
  for (unsigned int i = 0; i < num_elements; i++) {
    if (!current->next) {
      current->next = (struct arts_local_delayed_edt_s *)arts_calloc(
          1, sizeof(struct arts_local_delayed_edt_s));
      if (!current->next) {
        ARTS_ERROR("DB local delayed EDT allocation failed");
      }
    }
    current = current->next;
  }
  current->edt[element_pos] = edt;
  current->slot[element_pos] = slot;
  current->mode[element_pos] = mode;
}

static arts_guid_t arts_candidate_edt_guid(struct arts_edt_s *edt,
                                           arts_guid_t edt_guid) {
  if (edt_guid == NULL_GUID && edt) {
    return edt->current_edt;
  }
  return edt_guid;
}

static bool arts_same_local_writer(struct arts_db_frontier_s *frontier,
                                   struct arts_edt_s *edt,
                                   arts_guid_t edt_guid) {
  edt_guid = arts_candidate_edt_guid(edt, edt_guid);
  if (edt && frontier->localWriteEdt == edt) {
    return true;
  }
  return edt_guid != NULL_GUID && frontier->localWriteEdtGuid == edt_guid;
}

static bool arts_try_join_same_local_writer(
    struct arts_db_frontier_s *frontier, struct arts_edt_s *edt,
    arts_guid_t edt_guid) {
  bool same_writer = false;
  frontier_lock(&frontier->lock);
  same_writer = arts_same_local_writer(frontier, edt, edt_guid);
  frontier_unlock(&frontier->lock);
  return same_writer;
}

static void arts_record_local_writer(struct arts_db_frontier_s *frontier,
                                     struct arts_edt_s *edt,
                                     arts_guid_t edt_guid) {
  frontier->localWriteEdt = edt;
  frontier->localWriteEdtGuid = arts_candidate_edt_guid(edt, edt_guid);
}

static void arts_validate_db_slice(struct arts_db_s *db, uint64_t offset,
                                   uint64_t size) {
  uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
  if (size > UINT_MAX) {
    ARTS_ERROR("ESD slice size %lu exceeds DB_MODE_PTR transport limit",
               (unsigned long)size);
  }
  if (offset > data_size || size > data_size - offset) {
    ARTS_ERROR("ESD slice [%lu, %lu) is out of bounds for DB[Guid:%lu, Size:%lu]",
               (unsigned long)offset, (unsigned long)(offset + size), db->guid,
               (unsigned long)data_size);
  }
}

static unsigned int arts_db_slice_signal_size(struct arts_db_s *db,
                                              uint64_t slice_size,
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
  return (unsigned int)slice_size;
}

static void *arts_alloc_db_slice_copy(struct arts_db_s *db, uint64_t offset,
                                      uint64_t size, uint32_t flags) {
  arts_validate_db_slice(db, offset, size);
  unsigned int signal_size = arts_db_slice_signal_size(db, size, flags);
  void *copy = (flags & ARTS_DEP_FLAG_PRESERVE_SHAPE)
                   ? arts_calloc(1, signal_size)
                   : arts_malloc(signal_size);
  if (size) {
    char *dst = (char *)copy;
    if ((flags & ARTS_DEP_FLAG_PRESERVE_SHAPE) != 0) {
      dst += offset;
    }
    memcpy(dst, ((char *)(db + 1)) + offset, (size_t)size);
  }
  return copy;
}

static void arts_satisfy_local_edt_slice(struct arts_edt_s *edt,
                                         unsigned int slot,
                                         struct arts_db_s *db,
                                         uint64_t offset, uint64_t size,
                                         uint32_t flags) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  depv[slot].guid = db->guid;
  depv[slot].ptr = arts_alloc_db_slice_copy(db, offset, size, flags);
  depv[slot].mode = DB_MODE_PTR;
  depv[slot].slice_offset = 0;
  depv[slot].slice_size = 0;
  if (arts_atomic_sub(&edt->depc_needed, 1U) == 0) {
    arts_handle_remote_stolen_edt(edt);
  }
}

void arts_push_delayed_slice_request(struct arts_delayed_slice_request_s *head,
                                     unsigned int position,
                                     struct arts_edt_s *edt,
                                     arts_guid_t edt_guid, unsigned int slot,
                                     uint64_t offset, uint64_t size,
                                     uint32_t flags) {
  if (!head) {
    return;
  }
  unsigned int num_elements = position / DBSPERELEMENT;
  unsigned int element_pos = position % DBSPERELEMENT;
  struct arts_delayed_slice_request_s *current = head;
  for (unsigned int i = 0; i < num_elements; i++) {
    if (!current->next) {
      current->next = (struct arts_delayed_slice_request_s *)arts_calloc(
          1, sizeof(struct arts_delayed_slice_request_s));
      if (!current->next) {
        ARTS_ERROR("DB delayed slice request allocation failed");
      }
    }
    current = current->next;
  }
  current->edt[element_pos] = edt;
  current->edt_guid[element_pos] = edt_guid;
  current->slot[element_pos] = slot;
  current->flags[element_pos] = flags;
  current->offset[element_pos] = offset;
  current->size[element_pos] = size;
}

static void arts_signal_db_slice(struct arts_db_s *db, arts_guid_t edt_guid,
                                 unsigned int slot, uint64_t offset,
                                 uint64_t size, uint32_t flags) {
  arts_validate_db_slice(db, offset, size);
  if ((flags & ARTS_DEP_FLAG_PRESERVE_SHAPE) != 0) {
    unsigned int signal_size = arts_db_slice_signal_size(db, size, flags);
    void *copy = arts_alloc_db_slice_copy(db, offset, size, flags);
    arts_signal_edt_ptr_with_guid(edt_guid, slot, db->guid, copy, signal_size);
    arts_free(copy);
    return;
  }
  arts_signal_edt_ptr_with_guid(edt_guid, slot, db->guid,
                                (void *)(((char *)(db + 1)) + offset),
                                (unsigned int)size);
}

bool arts_push_db_to_frontier(struct arts_db_frontier_s *frontier,
                              unsigned int data, bool write, bool local,
                              bool bypass, struct arts_edt_s *edt,
                              arts_guid_t edt_guid, unsigned int slot,
                              arts_db_access_mode_t mode, bool *unique) {
  if (write && !bypass &&
      (frontier->roReadersCount > 0 || frontier->localRoReadersPos > 0 ||
       frontier->localRoPending > 0 || frontier->roOutstanding > 0)) {
    ARTS_TRACE_RDMA("cdag push skip local-ro db_frontier=%p edt=%lu "
                    "ro_count=%u local_ro_pos=%u pending=%u outstanding=%u",
                    (void *)frontier, arts_candidate_edt_guid(edt, edt_guid),
                    frontier->roReadersCount, frontier->localRoReadersPos,
                    frontier->localRoPending, frontier->roOutstanding);
    *unique = false;
    return false;
  }
  if (bypass) {
    if (!frontier_add_read_lock(&frontier->lock)) {
      return false;
    }
  } else if (write && !frontier_add_write_lock(&frontier->lock)) {
    return false;
  } else if (!write && !frontier_add_read_lock(&frontier->lock)) {
    return false;
  }

  bool inserted =
      arts_push_db_to_element(&frontier->list, frontier->position, data);
  if (inserted) {
    frontier->position++;
  }
  *unique = inserted;

  if (inserted && (write && !local)) {
    frontier->exNode = data;
    frontier->exEdtGuid = edt_guid;
    frontier->exEdt = edt;
    frontier->exSlot = slot;
    frontier->exMode = mode;
  }
  if (write && local) {
    arts_record_local_writer(frontier, edt, edt_guid);
  }

  frontier_unlock(&frontier->lock);
  return true;
}

bool arts_push_db_to_list(struct arts_db_list_s *db_list, unsigned int data,
                          bool write, bool local, bool bypass,
                          struct arts_edt_s *edt, arts_guid_t edt_guid,
                          unsigned int slot, arts_db_access_mode_t mode,
                          bool *on_head) {
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }
  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool inserted = false;
  bool unique = true;
  bool is_head = true;
  struct arts_db_frontier_s *accepted_frontier = NULL;
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    if (arts_push_db_to_frontier(frontier, data, write, local, bypass, edt,
                                 edt_guid, slot, mode, &unique)) {
      inserted = true;
      accepted_frontier = frontier;
      break;
    }
    if (local && arts_try_join_same_local_writer(frontier, edt, edt_guid)) {
      inserted = true;
      unique = false;
      accepted_frontier = frontier;
      break;
    }
    is_head = false;
    if (!frontier->next) {
      struct arts_db_frontier_s *new_frontier = arts_new_db_frontier();
      if (arts_atomic_cswap_ptr((volatile void **)&frontier->next, NULL,
                                new_frontier)) {
        arts_delete_db_frontier(new_frontier);
        while (!frontier->next) {
          ARTS_SPIN_PAUSE();
        }
      } else {
        arts_db_list_try_advance_tail(db_list, frontier, new_frontier);
      }
    }
  }
  if (inserted && local && !is_head && accepted_frontier) {
    frontier_lock(&accepted_frontier->lock);
    arts_push_delayed_edt(&accepted_frontier->localDelayed,
                          accepted_frontier->localPosition++, edt, slot, mode);
    frontier_unlock(&accepted_frontier->lock);
  }
  if (inserted && !write && local && accepted_frontier) {
    arts_atomic_add(&accepted_frontier->roOutstanding, 1U);
  }
  if (on_head) {
    *on_head = inserted && is_head;
  }
  arts_reader_unlock(&db_list->reader);
  return inserted && unique;
}

void arts_retire_local_ro_reader(struct arts_db_s *db) {
  if (!db || !db->db_list || db->db_list == (void *)1) {
    return;
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *head = db_list->head;
  bool progress = false;
  if (head) {
    frontier_lock(&head->lock);
    bool reader_gen = (head->exEdt == NULL && head->exEdtGuid == NULL_GUID);
    if (reader_gen && head->roOutstanding > 0) {
      if (arts_atomic_sub(&head->roOutstanding, 1U) == 0 &&
          head->localRoPending == 0) {
        progress = true;
      }
    }
    frontier_unlock(&head->lock);
  }
  if (progress) {
    struct arts_db_frontier_s *tail = arts_db_list_pop_head_locked(db_list);
    struct arts_db_frontier_s *to_delete_head = NULL;
    struct arts_db_frontier_s *to_delete_tail = NULL;
    arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, tail);
    if (db_list->head) {
      arts_promote_ready_heads_locked(db_list, db, arts_global_rank_id,
                                      &to_delete_head, &to_delete_tail);
    }
    arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
    arts_writer_unlock(&db_list->writer);
    return;
  }
  arts_writer_unlock(&db_list->writer);
}

bool arts_register_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot,
                                    arts_db_access_mode_t mode) {
  if (!db || db->db_type == ARTS_DB_LOCAL || db->db_list == (void *)1) {
    return false;
  }
  if (!db->db_list) {
    struct arts_db_list_s *new_list = arts_new_db_list();
    if (arts_atomic_cswap_ptr((volatile void **)&db->db_list, NULL, new_list)) {
      arts_delete_db_list(new_list);
    }
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }

  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
  }
  if (arts_db_list_has_remote_ew_writer_locked(db_list, rank, edt_guid, slot,
                                               mode)) {
    ARTS_TRACE_RDMA("cdag prereg remote-ew duplicate db=%lu rank=%u edt=%lu "
                    "slot=%u mode=%u",
                    db->guid, rank, edt_guid, slot, mode);
    arts_writer_unlock(&db_list->writer);
    return true;
  }
  struct arts_db_frontier_s *frontier = arts_db_list_get_tail_locked(db_list);
  struct arts_db_frontier_s *target = frontier;
  bool tail_used =
      (frontier->position != 0 || frontier->roReadersCount != 0 ||
       frontier->localRoReadersPos != 0 || frontier->localRoPending != 0 ||
       frontier->localWriteEdtGuid != NULL_GUID ||
       frontier->exEdtGuid != NULL_GUID || frontier->exEdt != NULL ||
       (frontier->lock & WRITE_SET) != 0);
  if (tail_used) {
    target = arts_db_list_append_tail_locked(db_list);
  }

  bool inserted = arts_push_db_to_element(&target->list, target->position, rank);
  if (inserted) {
    target->position++;
  }
  target->exNode = rank;
  target->exEdtGuid = edt_guid;
  target->exEdt = NULL;
  target->exSlot = slot;
  target->exMode = mode;
  target->exPreRegistered = true;
  target->exDelivered = false;
  arts_atomic_fetch_or(&target->lock, WRITE_SET);
  arts_writer_unlock(&db_list->writer);
  return true;
}

bool arts_register_local_ew_writer(struct arts_db_s *db, arts_guid_t edt_guid) {
  if (!db || db->db_type == ARTS_DB_LOCAL || db->db_list == (void *)1) {
    return false;
  }
  if (!db->db_list) {
    struct arts_db_list_s *new_list = arts_new_db_list();
    if (arts_atomic_cswap_ptr((volatile void **)&db->db_list, NULL, new_list)) {
      arts_delete_db_list(new_list);
    }
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }
  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
  }
  if (arts_db_list_has_local_ew_writer_locked(db_list, edt_guid)) {
    ARTS_TRACE_RDMA("cdag prereg local-ew duplicate db=%lu edt=%lu", db->guid,
                    edt_guid);
    arts_writer_unlock(&db_list->writer);
    return true;
  }
  struct arts_db_frontier_s *frontier = arts_db_list_get_tail_locked(db_list);
  struct arts_db_frontier_s *target = frontier;
  bool tail_used = (frontier->position != 0 || frontier->roReadersCount != 0 ||
                    frontier->localRoReadersPos != 0 ||
                    frontier->localWriteEdtGuid != NULL_GUID ||
                    frontier->exEdtGuid != NULL_GUID ||
                    (frontier->lock & WRITE_SET) != 0);
  if (tail_used) {
    target = arts_db_list_append_tail_locked(db_list);
  }
  target->localWriteEdtGuid = edt_guid;
  target->localWriteEdt = NULL;
  arts_atomic_fetch_or(&target->lock, WRITE_SET);
  ARTS_TRACE_RDMA("cdag prereg local-ew db=%lu edt=%lu target=%p tail_used=%u",
                  db->guid, edt_guid, (void *)target, tail_used ? 1U : 0U);
  arts_writer_unlock(&db_list->writer);
  return true;
}

bool arts_register_local_ro_reader(struct arts_db_s *db, arts_guid_t edt_guid,
                                   unsigned int slot) {
  if (!db || db->db_type == ARTS_DB_LOCAL || db->db_list == (void *)1) {
    return false;
  }
  if (!db->db_list) {
    struct arts_db_list_s *new_list = arts_new_db_list();
    if (arts_atomic_cswap_ptr((volatile void **)&db->db_list, NULL, new_list)) {
      arts_delete_db_list(new_list);
    }
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }
  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
  }
  if (arts_db_list_has_local_ro_reader_locked(db_list, edt_guid, slot)) {
    ARTS_TRACE_RDMA("cdag prereg local-ro duplicate db=%lu edt=%lu slot=%u",
                    db->guid, edt_guid, slot);
    arts_writer_unlock(&db_list->writer);
    return true;
  }
  struct arts_db_frontier_s *frontier = arts_db_list_get_tail_locked(db_list);
  bool tail_is_writer = (frontier->exEdt != NULL ||
                         frontier->exEdtGuid != NULL_GUID ||
                         frontier->localWriteEdtGuid != NULL_GUID ||
                         (frontier->lock & WRITE_SET) != 0 ||
                         frontier->position != 0);
  struct arts_db_frontier_s *target = frontier;
  if (tail_is_writer) {
    target = arts_db_list_append_tail_locked(db_list);
  }
  arts_push_ro_reader(&target->localRoReaders, target->localRoReadersPos,
                      arts_global_rank_id, edt_guid, slot);
  target->localRoReadersPos++;
  target->localRoPending++;
  ARTS_TRACE_RDMA("cdag prereg local-ro db=%lu edt=%lu target=%p tail_writer=%u "
                  "pending=%u ro_count=%u",
                  db->guid, edt_guid, (void *)target, tail_is_writer ? 1U : 0U,
                  target->localRoPending, target->roReadersCount);
  arts_writer_unlock(&db_list->writer);
  return true;
}

bool arts_claim_local_ro_reader(struct arts_db_s *db, struct arts_edt_s *edt,
                                arts_guid_t edt_guid, unsigned int slot,
                                arts_db_access_mode_t mode, bool *on_head) {
  if (on_head) {
    *on_head = false;
  }
  if (!db || !db->db_list || db->db_list == (void *)1 ||
      edt_guid == NULL_GUID) {
    return false;
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_writer_lock(&db_list->reader, &db_list->writer);
  bool claimed = false;
  for (struct arts_db_frontier_s *frontier = db_list->head;
       frontier && !claimed; frontier = frontier->next) {
    if (frontier->localRoPending == 0) {
      continue;
    }
    struct arts_ro_reader_s *cur = &frontier->localRoReaders;
    unsigned int n = frontier->localRoReadersPos;
    for (unsigned int i = 0; i < n; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      if (cur->edt_guid[pos] == edt_guid && cur->slot[pos] == slot) {
        cur->edt_guid[pos] = NULL_GUID;
        frontier->localRoPending--;
        frontier_lock(&frontier->lock);
        arts_atomic_add(&frontier->roOutstanding, 1U);
        bool head_match = (frontier == db_list->head);
        if (!head_match) {
          arts_push_delayed_edt(&frontier->localDelayed,
                                frontier->localPosition++, edt, slot, mode);
        }
        frontier_unlock(&frontier->lock);
        if (head_match && on_head) {
          *on_head = true;
        }
        ARTS_TRACE_RDMA("cdag claim local-ro db=%lu edt=%lu frontier=%p "
                        "head=%u pending=%u outstanding=%u",
                        db->guid, edt_guid, (void *)frontier,
                        head_match ? 1U : 0U, frontier->localRoPending,
                        frontier->roOutstanding);
        claimed = true;
        break;
      }
      if (pos + 1 == DBSPERELEMENT) {
        cur = cur->next;
        if (!cur) {
          break;
        }
      }
    }
  }
  arts_writer_unlock(&db_list->writer);
  return claimed;
}

bool arts_claim_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                 arts_guid_t edt_guid, unsigned int slot,
                                 arts_db_access_mode_t mode, bool *on_head,
                                 bool *deliver) {
  if (on_head) {
    *on_head = false;
  }
  if (deliver) {
    *deliver = false;
  }
  if (!db || !db->db_list || db->db_list == (void *)1) {
    return false;
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool found = false;
  bool is_head = true;
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    if (arts_frontier_has_remote_ew_writer(frontier, rank, edt_guid, slot,
                                           mode)) {
      found = true;
      if (on_head) {
        *on_head = is_head;
      }
      if (is_head && !frontier->exDelivered) {
        frontier->exDelivered = true;
        if (deliver) {
          *deliver = true;
        }
      }
      frontier_unlock(&frontier->lock);
      break;
    }
    frontier_unlock(&frontier->lock);
    is_head = false;
  }
  arts_reader_unlock(&db_list->reader);
  return found;
}

bool arts_register_remote_ro_reader(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot) {
  if (!db || db->db_type == ARTS_DB_LOCAL || db->db_list == (void *)1) {
    return false;
  }
  if (!db->db_list) {
    struct arts_db_list_s *new_list = arts_new_db_list();
    if (arts_atomic_cswap_ptr((volatile void **)&db->db_list, NULL, new_list)) {
      arts_delete_db_list(new_list);
    }
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }

  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
  }
  if (arts_db_list_has_remote_ro_reader_locked(db_list, rank, edt_guid, slot,
                                               true, true)) {
    ARTS_TRACE_RDMA("cdag prereg remote-ro duplicate db=%lu rank=%u edt=%lu "
                    "slot=%u",
                    db->guid, rank, edt_guid, slot);
    arts_writer_unlock(&db_list->writer);
    return true;
  }
  bool registered = false;
  bool landed_on_head = false;
  struct arts_db_frontier_s *frontier = arts_db_list_get_tail_locked(db_list);
  bool tail_is_writer = (frontier->exEdt != NULL ||
                         frontier->exEdtGuid != NULL_GUID ||
                         (frontier->lock & WRITE_SET) != 0 ||
                         frontier->position != 0);
  struct arts_db_frontier_s *target = frontier;
  if (tail_is_writer) {
    target = arts_db_list_append_tail_locked(db_list);
  }
  arts_push_ro_reader(&target->roReaders, target->roReadersCount, rank, edt_guid,
                      slot);
  target->roReadersCount++;
  registered = true;
  landed_on_head = (target == db_list->head);
  if (landed_on_head) {
    frontier_lock(&target->lock);
    arts_serve_ro_readers(target, db);
    frontier_unlock(&target->lock);
    if (db_list->head && arts_reader_gen_drained(db_list->head)) {
      struct arts_db_frontier_s *drained =
          arts_db_list_pop_head_locked(db_list);
      if (db_list->head) {
        arts_promote_ready_heads_locked(db_list, db, arts_global_rank_id,
                                        &to_delete_head, &to_delete_tail);
      }
      arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, drained);
    }
  }
  arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
  arts_writer_unlock(&db_list->writer);
  return registered;
}

bool arts_remote_ro_reader_preregistered(struct arts_db_s *db,
                                         unsigned int rank,
                                         arts_guid_t edt_guid) {
  if (!db || !db->db_list || db->db_list == (void *)1) {
    return false;
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool found = arts_db_list_has_remote_ro_reader_locked(
      db_list, rank, edt_guid, 0, edt_guid != NULL_GUID, false);
  arts_reader_unlock(&db_list->reader);
  return found;
}

bool arts_remote_ro_reader_preregistered_exact(struct arts_db_s *db,
                                               unsigned int rank,
                                               arts_guid_t edt_guid,
                                               unsigned int slot) {
  if (!db || !db->db_list || db->db_list == (void *)1 ||
      edt_guid == NULL_GUID) {
    return false;
  }
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool found = arts_db_list_has_remote_ro_reader_locked(
      db_list, rank, edt_guid, slot, true, true);
  arts_reader_unlock(&db_list->reader);
  return found;
}

static void arts_serve_ro_readers(struct arts_db_frontier_s *frontier,
                                  struct arts_db_s *db) {
  if (!frontier->roReadersCount || frontier->roReadersServed) {
    return;
  }
  frontier->roReadersServed = true;
  unsigned int payload =
      (unsigned int)(db->header.size - sizeof(struct arts_db_s));
  void *src = (void *)(db + 1);
  uint64_t trace_value = 0;
  if (payload >= sizeof(trace_value)) {
    memcpy(&trace_value, src, sizeof(trace_value));
  }
  ARTS_TRACE_RDMA("cdag serve-ro db=%lu frontier=%p readers=%u value=%lu "
                  "local_pending=%u outstanding=%u",
                  db->guid, (void *)frontier, frontier->roReadersCount,
                  trace_value, frontier->localRoPending,
                  frontier->roOutstanding);
  unsigned int n = frontier->roReadersCount;
  struct arts_ro_reader_s *cur = &frontier->roReaders;
  for (unsigned int i = 0; i < n; i++) {
    unsigned int pos = i % DBSPERELEMENT;
    unsigned int node = cur->node[pos];
    arts_guid_t edt_guid = cur->edt_guid[pos];
    unsigned int slot = cur->slot[pos];
    if (node == arts_global_rank_id) {
      arts_signal_edt_ptr_with_guid(edt_guid, slot, db->guid, src, payload);
    } else {
      arts_remote_signal_edt_with_ptr(edt_guid, db->guid, src, payload, slot);
    }
    if (pos + 1 == DBSPERELEMENT) {
      cur = cur->next;
      if (!cur) {
        break;
      }
    }
  }
}

bool arts_request_db_slice(struct arts_db_s *db, struct arts_edt_s *local_edt,
                           arts_guid_t edt_guid, unsigned int slot,
                           uint64_t offset, uint64_t size, uint32_t flags) {
  if (!db) {
    return false;
  }

  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  if (!db_list) {
    if (local_edt) {
      arts_satisfy_local_edt_slice(local_edt, slot, db, offset, size, flags);
    } else {
      arts_signal_db_slice(db, edt_guid, slot, offset, size, flags);
    }
    return true;
  }

  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_db_list_take_frontier_locked(db_list);
      arts_writer_unlock(&db_list->writer);
    }
  }

  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool inserted = false;
  bool is_head = true;
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    if (frontier_add_read_lock(&frontier->lock)) {
      if (is_head) {
        if (local_edt) {
          arts_satisfy_local_edt_slice(local_edt, slot, db, offset, size,
                                       flags);
        } else {
          arts_signal_db_slice(db, edt_guid, slot, offset, size, flags);
        }
      } else {
        arts_push_delayed_slice_request(&frontier->sliceDelayed,
                                        frontier->slicePosition++, local_edt,
                                        edt_guid, slot, offset, size, flags);
      }
      frontier_unlock(&frontier->lock);
      inserted = true;
      break;
    }
    is_head = false;
    if (!frontier->next) {
      struct arts_db_frontier_s *new_frontier = arts_new_db_frontier();
      if (arts_atomic_cswap_ptr((volatile void **)&frontier->next, NULL,
                                new_frontier)) {
        arts_delete_db_frontier(new_frontier);
        while (!frontier->next) {
          ARTS_SPIN_PAUSE();
        }
      } else {
        arts_db_list_try_advance_tail(db_list, frontier, new_frontier);
      }
    }
  }
  arts_reader_unlock(&db_list->reader);
  return inserted;
}

unsigned int arts_current_frontier_size(struct arts_db_list_s *db_list) {
  unsigned int size = 0U;
  arts_reader_lock(&db_list->reader, &db_list->writer);
  if (db_list->head) {
    frontier_lock(&db_list->head->lock);
    size = db_list->head->position;
    frontier_unlock(&db_list->head->lock);
  }
  arts_reader_unlock(&db_list->reader);
  return size;
}

bool arts_db_frontier_iter_init(struct arts_db_frontier_iterator_s *iter,
                                struct arts_db_frontier_s *frontier) {
  if (!frontier || !frontier->position) {
    return false;
  }
  *iter = (struct arts_db_frontier_iterator_s){
      .frontier = frontier,
      .currentElement = &frontier->list,
  };
  return true;
}

unsigned int
arts_db_frontier_iter_size(struct arts_db_frontier_iterator_s *iter) {
  return iter->frontier->position;
}

bool arts_db_frontier_iter_next(struct arts_db_frontier_iterator_s *iter,
                                unsigned int *next) {
  if (iter->currentIndex < iter->frontier->position) {
    *next = iter->currentElement->array[iter->currentIndex++ % DBSPERELEMENT];
    if (!(iter->currentIndex % DBSPERELEMENT)) {
      iter->currentElement = iter->currentElement->next;
    }
    return true;
  }
  return false;
}

bool arts_db_frontier_iter_has_next(struct arts_db_frontier_iterator_s *iter) {
  return (iter->currentIndex < iter->frontier->position);
}

bool arts_close_frontier(struct arts_db_list_s *db_list,
                         struct arts_db_frontier_iterator_s *iter) {
  bool valid = false;
  arts_reader_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *frontier = db_list->head;
  if (frontier) {
    frontier_lock(&frontier->lock);

    arts_atomic_fetch_or(&frontier->lock, WRITE_SET | 1U);
    valid = arts_db_frontier_iter_init(iter, frontier);

    frontier_unlock(&frontier->lock);
  }
  arts_reader_unlock(&db_list->reader);
  return valid;
}

void arts_signal_frontier_remote(struct arts_db_frontier_s *frontier,
                                 struct arts_db_s *db, unsigned int get_from) {
  frontier_lock(&frontier->lock);

  /* Idempotent head signal: a generation may be driven to head by both a
   * predecessor's retirement and self-promotion over a settled frontier. */
  if (frontier->headSignaled) {
    frontier_unlock(&frontier->lock);
    return;
  }
  frontier->headSignaled = true;

  if ((frontier->exEdt || frontier->exEdtGuid != NULL_GUID) &&
      (!frontier->exPreRegistered || !frontier->exDelivered)) {
    arts_guid_t edt_guid = frontier->exEdtGuid;
    if (edt_guid == NULL_GUID && frontier->exEdt) {
      edt_guid = frontier->exEdt->current_edt;
    }
    if (frontier->exPreRegistered) {
      frontier->exDelivered = true;
    }
    if (frontier->exNode == get_from) {
      arts_remote_send_already_local((int)get_from, db->guid, edt_guid,
                                     frontier->exSlot, frontier->exMode);
    } else if (frontier->exNode != arts_global_rank_id) {
      arts_remote_db_forward_full((int)frontier->exNode, (int)get_from,
                                  db->guid, edt_guid, (int)frontier->exSlot,
                                  frontier->exMode);
    } else {
      arts_remote_db_forward_full((int)arts_global_rank_id, (int)get_from,
                                  db->guid, edt_guid, (int)frontier->exSlot,
                                  frontier->exMode);
    }
  }

  struct arts_db_frontier_iterator_s iter;
  if (arts_db_frontier_iter_init(&iter, frontier)) {
    unsigned int node;
    while (arts_db_frontier_iter_next(&iter, &node)) {
      if (node != arts_global_rank_id &&
          !((frontier->exEdt || frontier->exEdtGuid != NULL_GUID) &&
            node == frontier->exNode)) {
        arts_remote_db_forward((int)node, (int)get_from, db->guid, DB_MODE_RO,
                               0); // Don't care about mode
      }
    }
  }

  if (frontier->localPosition) {
    struct arts_local_delayed_edt_s *current = &frontier->localDelayed;
    for (unsigned int i = 0; i < frontier->localPosition; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      struct arts_edt_s *edt = current->edt[pos];
      unsigned int slot = current->slot[pos];
      arts_remote_db_request(db->guid, (int)get_from, edt, (int)slot,
                             current->mode[pos], 0, true);
      if (pos + 1 == DBSPERELEMENT) {
        current = current->next;
      }
    }
  }

  if (frontier->slicePosition) {
    struct arts_delayed_slice_request_s *current = &frontier->sliceDelayed;
    for (unsigned int i = 0; i < frontier->slicePosition; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      if (current->edt[pos]) {
        arts_satisfy_local_edt_slice(current->edt[pos], current->slot[pos], db,
                                     current->offset[pos], current->size[pos],
                                     current->flags[pos]);
      } else {
        arts_signal_db_slice(db, current->edt_guid[pos], current->slot[pos],
                             current->offset[pos], current->size[pos],
                             current->flags[pos]);
      }
      if (pos + 1 == DBSPERELEMENT) {
        current = current->next;
      }
    }
  }

  if (arts_push_db_to_element(&frontier->list, frontier->position, get_from)) {
    frontier->position++;
  }
  /* Targeted per-generation snapshot for pre-registered remote RO readers. */
  arts_serve_ro_readers(frontier, db);
  frontier_unlock(&frontier->lock);
}

void arts_signal_frontier_local(struct arts_db_frontier_s *frontier,
                                struct arts_db_s *db) {
  frontier_lock(&frontier->lock);

  /* Idempotent head signal: a generation may be driven to head by both a
   * predecessor's retirement and self-promotion over a settled frontier. */
  if (frontier->headSignaled) {
    frontier_unlock(&frontier->lock);
    return;
  }
  frontier->headSignaled = true;

  if (frontier->exEdt || frontier->exEdtGuid != NULL_GUID) {
    arts_guid_t edt_guid = frontier->exEdtGuid;
    struct arts_edt_s *edt = frontier->exEdt;
    if (edt_guid == NULL_GUID && edt) {
      edt_guid = edt->current_edt;
    }
    if (!edt && edt_guid != NULL_GUID) {
      edt = (struct arts_edt_s *)arts_route_table_lookup_item(edt_guid);
    }
    if (frontier->exNode == arts_global_rank_id) {
      if (edt) {
        // TODO(gpu): GPU EDTs need GPU memory, not this CPU pointer.
        arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
      arts_route_table_lookup_db(db->guid, NULL, false);
        depv[frontier->exSlot].ptr = db + 1;
        if (arts_atomic_sub(&edt->depc_needed, 1U) == 0) {
          arts_handle_remote_stolen_edt(edt);
        }
      } else {
        ARTS_INFO("Local frontier missing EDT[Guid:%lu] on rank %u", edt_guid,
                  arts_global_rank_id);
      }
    } else if (!frontier->exPreRegistered || !frontier->exDelivered) {
      /* Pre-registered writers may already have been shipped by their own
       * late full request (arts_claim_remote_ew_writer). Ship here only if
       * delivery has not already been claimed, and claim it now. */
      frontier->exDelivered = true;
      arts_remote_db_full_send_now((int)frontier->exNode, db, edt_guid,
                                   frontier->exSlot, frontier->exMode);
    }
  }

  if (frontier->localPosition) {
    struct arts_local_delayed_edt_s *current = &frontier->localDelayed;
    for (unsigned int i = 0; i < frontier->localPosition; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      struct arts_edt_s *edt = current->edt[pos];
      // TODO(gpu): GPU EDTs need GPU memory, not this CPU pointer.
      arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
      arts_route_table_lookup_db(db->guid, NULL, false);
      depv[current->slot[pos]].ptr = db + 1;

      if (arts_atomic_sub(&edt->depc_needed, 1U) == 0) {
        arts_handle_remote_stolen_edt(edt);
      }

      if (pos + 1 == DBSPERELEMENT) {
        current = current->next;
      }
    }
  }

  if (frontier->slicePosition) {
    struct arts_delayed_slice_request_s *current = &frontier->sliceDelayed;
    for (unsigned int i = 0; i < frontier->slicePosition; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      if (current->edt[pos]) {
        arts_satisfy_local_edt_slice(current->edt[pos], current->slot[pos], db,
                                     current->offset[pos], current->size[pos],
                                     current->flags[pos]);
      } else {
        arts_signal_db_slice(db, current->edt_guid[pos], current->slot[pos],
                             current->offset[pos], current->size[pos],
                             current->flags[pos]);
      }
      if (pos + 1 == DBSPERELEMENT) {
        current = current->next;
      }
    }
  }
  arts_serve_ro_readers(frontier, db);
  frontier_unlock(&frontier->lock);
}

static bool arts_reader_gen_drained(struct arts_db_frontier_s *frontier) {
  if (!frontier) {
    return false;
  }
  bool drained = false;
  frontier_lock(&frontier->lock);
  bool reader_gen =
      (frontier->exEdt == NULL && frontier->exEdtGuid == NULL_GUID);
  if (reader_gen && frontier->roReadersCount > 0 &&
      frontier->roReadersServed && frontier->localRoPending == 0 &&
      frontier->roOutstanding == 0) {
    drained = true;
  }
  frontier_unlock(&frontier->lock);
  return drained;
}

static void arts_queue_frontier_delete(
    struct arts_db_frontier_s **to_delete_head,
    struct arts_db_frontier_s **to_delete_tail,
    struct arts_db_frontier_s *frontier) {
  if (!frontier) {
    return;
  }
  frontier->next = NULL;
  if (!*to_delete_head) {
    *to_delete_head = *to_delete_tail = frontier;
  } else {
    (*to_delete_tail)->next = frontier;
    *to_delete_tail = frontier;
  }
}

static void arts_apply_remote_update_payload(struct arts_db_s *db,
                                             const void *payload,
                                             uint64_t payload_size) {
  uint64_t expected = db->header.size - sizeof(struct arts_db_s);
  if (payload_size != expected) {
    ARTS_ERROR("Remote DB update size mismatch DB[Guid:%lu] expected=%lu "
               "received=%lu",
               db->guid, expected, payload_size);
  }
  if (expected) {
    memcpy((void *)(db + 1), payload, expected);
  }
  arts_route_table_set_cache_rank(db->guid, (int)arts_global_rank_id);
}

static void arts_stage_remote_update_locked(struct arts_db_frontier_s *frontier,
                                            const void *payload,
                                            uint64_t payload_size) {
  if (frontier->exUpdatePending || frontier->exUpdatePayload) {
    ARTS_ERROR("Duplicate staged remote DB update for writer EDT[Guid:%lu]",
               frontier->exEdtGuid);
  }
  frontier->exUpdatePayload = arts_malloc(payload_size ? payload_size : 1U);
  if (!frontier->exUpdatePayload) {
    ARTS_ERROR("Remote DB update allocation failed for writer EDT[Guid:%lu]",
               frontier->exEdtGuid);
  }
  if (payload_size) {
    memcpy(frontier->exUpdatePayload, payload, payload_size);
  }
  frontier->exUpdateSize = payload_size;
  frontier->exUpdatePending = true;
}

static bool
arts_apply_pending_remote_writer_locked(struct arts_db_frontier_s *frontier,
                                        struct arts_db_s *db) {
  bool applied = false;
  frontier_lock(&frontier->lock);
  if (frontier->exUpdatePending) {
    arts_apply_remote_update_payload(db, frontier->exUpdatePayload,
                                     frontier->exUpdateSize);
    arts_free(frontier->exUpdatePayload);
    frontier->exUpdatePayload = NULL;
    frontier->exUpdateSize = 0;
    frontier->exUpdatePending = false;
    applied = true;
  }
  frontier_unlock(&frontier->lock);
  return applied;
}

static void arts_promote_ready_heads_locked(
    struct arts_db_list_s *db_list, struct arts_db_s *db, unsigned int rank,
    struct arts_db_frontier_s **to_delete_head,
    struct arts_db_frontier_s **to_delete_tail) {
  while (db_list->head) {
    if (arts_apply_pending_remote_writer_locked(db_list->head, db)) {
      struct arts_db_frontier_s *applied =
          arts_db_list_pop_head_locked(db_list);
      arts_queue_frontier_delete(to_delete_head, to_delete_tail, applied);
      rank = arts_global_rank_id;
      continue;
    }

    if (rank == arts_global_rank_id) {
      arts_signal_frontier_local(db_list->head, db);
    } else {
      arts_signal_frontier_remote(db_list->head, db, rank);
    }
    if (!arts_reader_gen_drained(db_list->head)) {
      break;
    }

    struct arts_db_frontier_s *drained =
        arts_db_list_pop_head_locked(db_list);
    arts_queue_frontier_delete(to_delete_head, to_delete_tail, drained);
  }
}

bool arts_apply_remote_writer_update(struct arts_db_s *db, unsigned int rank,
                                     arts_guid_t edt_guid,
                                     const void *payload,
                                     uint64_t payload_size) {
  if (!db || !db->db_list || db->db_list == (void *)1 ||
      edt_guid == NULL_GUID) {
    return false;
  }

  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  bool found = false;

  arts_writer_lock(&db_list->reader, &db_list->writer);
  bool is_head = true;
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    bool match = (frontier->exNode == rank && frontier->exEdtGuid == edt_guid);
    if (match) {
      found = true;
      if (frontier->exUpdatePending) {
        ARTS_ERROR("Duplicate remote DB update for head writer EDT[Guid:%lu]",
                   edt_guid);
      }
      if (is_head) {
        arts_apply_remote_update_payload(db, payload, payload_size);
        frontier_unlock(&frontier->lock);
        struct arts_db_frontier_s *retired =
            arts_db_list_pop_head_locked(db_list);
        arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, retired);
        arts_promote_ready_heads_locked(db_list, db, arts_global_rank_id,
                                        &to_delete_head, &to_delete_tail);
      } else {
        arts_stage_remote_update_locked(frontier, payload, payload_size);
        frontier_unlock(&frontier->lock);
      }
      break;
    }
    frontier_unlock(&frontier->lock);
    is_head = false;
  }
  arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
  arts_writer_unlock(&db_list->writer);
  return found;
}

bool arts_apply_remote_writer_put(struct arts_db_s *db, unsigned int rank,
                                  arts_guid_t edt_guid, uint64_t offset,
                                  const void *payload, uint64_t payload_size) {
  if (!db || !db->db_list || db->db_list == (void *)1 ||
      edt_guid == NULL_GUID) {
    return false;
  }

  uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
  if (offset > data_size || payload_size > data_size - offset) {
    ARTS_ERROR("Remote DB put size mismatch DB[Guid:%lu] offset=%lu size=%lu "
               "payload=%lu",
               db->guid, offset, data_size, payload_size);
  }

  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  bool applied = false;

  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *head = db_list->head;
  if (head) {
    frontier_lock(&head->lock);
    bool match = (head->exNode == rank && head->exEdtGuid == edt_guid &&
                  head->exMode == DB_MODE_EW);
    if (match) {
      if (payload_size) {
        memcpy(((char *)(db + 1)) + offset, payload, payload_size);
      }
      arts_route_table_set_cache_rank(db->guid, (int)arts_global_rank_id);
      frontier_unlock(&head->lock);
      struct arts_db_frontier_s *retired =
          arts_db_list_pop_head_locked(db_list);
      arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, retired);
      arts_promote_ready_heads_locked(db_list, db, arts_global_rank_id,
                                      &to_delete_head, &to_delete_tail);
      applied = true;
    } else {
      frontier_unlock(&head->lock);
    }
  }
  arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
  arts_writer_unlock(&db_list->writer);
  return applied;
}

bool arts_release_remote_writer(struct arts_db_s *db, unsigned int rank,
                                arts_guid_t edt_guid) {
  if (!db || !db->db_list || db->db_list == (void *)1 ||
      edt_guid == NULL_GUID) {
    return false;
  }

  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  bool released = false;

  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *head = db_list->head;
  if (head) {
    frontier_lock(&head->lock);
    bool match = (head->exNode == rank && head->exEdtGuid == edt_guid &&
                  head->exMode == DB_MODE_EW);
    if (match) {
      frontier_unlock(&head->lock);
      struct arts_db_frontier_s *retired =
          arts_db_list_pop_head_locked(db_list);
      arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, retired);
      arts_promote_ready_heads_locked(db_list, db, arts_global_rank_id,
                                      &to_delete_head, &to_delete_tail);
      released = true;
    } else {
      frontier_unlock(&head->lock);
    }
  }
  arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
  arts_writer_unlock(&db_list->writer);
  return released;
}

void arts_progress_frontier(struct arts_db_s *db, unsigned int rank) {
  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_writer_lock(&db_list->reader, &db_list->writer);
  /*
   * Pop the retiring generation, promote+signal the next, then drain any
   * served reader-only generations (they have no consumer left to retire
   * them) so the following writer is reached in one pass — all under the
   * single writer lock we already hold.
   */
  struct arts_db_frontier_s *tail = NULL;
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  if (db_list->head) {
    tail = arts_db_list_pop_head_locked(db_list);
    arts_queue_frontier_delete(&to_delete_head, &to_delete_tail, tail);
    arts_promote_ready_heads_locked(db_list, db, rank, &to_delete_head,
                                    &to_delete_tail);
  }
  arts_db_list_recycle_frontier_queue_locked(db_list, to_delete_head);
  arts_writer_unlock(&db_list->writer);
}

bool arts_progress_and_get_frontier(struct arts_db_list_s *db_list,
                                    struct arts_db_frontier_iterator_s *iter) {
  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *tail = arts_db_list_pop_head_locked(db_list);
  arts_writer_unlock(&db_list->writer);
  // This should be safe since the writer lock ensures all readers are done
  return arts_db_frontier_iter_init(iter, tail);
}
