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

/* Forward declarations for mutually-referenced frontier helpers. */
void arts_signal_frontier_local(struct arts_db_frontier_s *frontier,
                                struct arts_db_s *db);
void arts_signal_frontier_remote(struct arts_db_frontier_s *frontier,
                                 struct arts_db_s *db, unsigned int get_from);
static void arts_serve_ro_readers(struct arts_db_frontier_s *frontier,
                                  struct arts_db_s *db);
static bool arts_reader_gen_drained(struct arts_db_frontier_s *frontier);

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

// This should be done before being released into the wild
struct arts_db_list_s *arts_new_db_list() {
  struct arts_db_list_s *ret =
      (struct arts_db_list_s *)arts_calloc(1, sizeof(struct arts_db_list_s));
  if (!ret) {
    ARTS_ERROR("DB list allocation failed");
  }
  ret->head = ret->tail = arts_new_db_frontier();
  return ret;
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
  arts_free(frontier);
}

void arts_delete_db_list(struct arts_db_list_s *db_list) {
  if (!db_list) {
    return;
  }
  struct arts_db_frontier_s *frontier = db_list->head;
  while (frontier) {
    struct arts_db_frontier_s *next = frontier->next;
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
    arts_free(frontier);
    frontier = next;
  }
  arts_free(db_list);
}

/* Append a remote RO reader (node, edt_guid, slot) to a generation's list. */
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
  /*
   * ESD is intentionally copy-based transport: consumers see only the RO
   * byte range they asked for, while ARTS retains whole-DB ownership.
   */
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
  /*
   * A writer must never fuse into a generation that carries owner-local RO
   * readers pre-registered in CDAG order (single-node eager prereg): it must
   * seek a strictly-later generation so this version's readers drain first.
   * Reject here so the caller advances past the reserved reader generation and
   * (for a pre-registered local writer) joins its own sealed generation via
   * arts_try_join_same_local_writer. localRoReadersPos is set during phase-1
   * prereg before any consumer EDT runs and is monotonic, so this unlocked read
   * is stable; it is always 0 on multinode, leaving that path unchanged.
   */
  if (write && !bypass && frontier->localRoReadersPos > 0) {
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

// Returns if the push is to the head frontier
/* A read after write from the same node would send duplicate copies of DB.
 * To fix this, if the node is remote, we only return true if the adding the
 * rank to the frontier is unique.  If the db is local then we return if the DB
 * is added to the first frontier reguardless of if there are duplicates.
 */
/*
 * arts_push_db_to_list — Register a rank/EDT in the DB's frontier list.
 *
 * Tries each frontier from head to tail until one accepts the push (i.e.
 * the frontier's lock allows the requested access mode).  The first
 * frontier attempted is always db_list->head (the "current" frontier).
 *
 * on_head (out, optional): set to true if the push landed on the head
 *   frontier, false if a later frontier was used.  Callers use this to
 *   decide whether acquire_dbs should decrement depc_needed directly
 *   (head) or defer to frontier signaling (non-head).
 *
 * Returns true if the rank was inserted uniquely.
 */
bool arts_push_db_to_list(struct arts_db_list_s *db_list, unsigned int data,
                          bool write, bool local, bool bypass,
                          struct arts_edt_s *edt, arts_guid_t edt_guid,
                          unsigned int slot, arts_db_access_mode_t mode,
                          bool *on_head) {
  if (!db_list->head) {
    if (arts_writer_try_lock(&db_list->reader, &db_list->writer)) {
      db_list->head = db_list->tail = arts_new_db_frontier();
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
      }
    }
  }
  if (inserted && local && !is_head && accepted_frontier) {
    /* Non-head local acquires are satisfied later when this frontier becomes
     * head. Protect the delayed-EDT append with the frontier lock: the DB-list
     * reader lock is shared and does not serialize concurrent local acquires
     * targeting the same accepted frontier. */
    frontier_lock(&accepted_frontier->lock);
    arts_push_delayed_edt(&accepted_frontier->localDelayed,
                          accepted_frontier->localPosition++, edt, slot, mode);
    frontier_unlock(&accepted_frontier->lock);
  }
  /*
   * Owner-local RO readers must retire their (reader-only) generation
   * themselves: no writer update will progress it. Track the count here so the
   * last reader to release drives arts_retire_local_ro_reader. Count both head
   * and non-head readers; head readers run promptly but still hold the
   * generation open against the following writer until they complete.
   */
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
    /* Only reader-only generations are retired this way; a generation owned by
     * an exclusive writer is retired by that writer's update. */
    bool reader_gen = (head->exEdt == NULL && head->exEdtGuid == NULL_GUID);
    if (reader_gen && head->roOutstanding > 0) {
      /* Hold the generation open while any pre-registered local reader has not
       * yet joined (localRoPending > 0): a strictly-later writer must not be
       * promoted until every owner-local reader of this version has acquired. */
      if (arts_atomic_sub(&head->roOutstanding, 1U) == 0 &&
          head->localRoPending == 0) {
        progress = true;
      }
    }
    frontier_unlock(&head->lock);
  }
  if (progress) {
    /* Inline the pop+signal under the writer lock we already hold, mirroring
     * arts_progress_frontier (which would re-take the writer lock). */
    struct arts_db_frontier_s *tail = db_list->head;
    db_list->head = db_list->head->next;
    if (db_list->head) {
      arts_signal_frontier_local(db_list->head, db);
    }
    arts_writer_unlock(&db_list->writer);
    if (tail) {
      arts_delete_db_frontier(tail);
    }
    return;
  }
  arts_writer_unlock(&db_list->writer);
}

/*
 * Pre-register a remote EW writer in CDAG order. The writer's frontier
 * generation is reserved here, on the owner, at arts_add_dependence time. We
 * take a write lock on the first frontier that has no writer yet (creating a
 * fresh generation as needed), then stamp the exclusive-writer slot exactly as
 * arts_push_db_to_frontier would for a real remote write, plus exPreRegistered.
 * Delivery (shipping the DB to the writer) is intentionally NOT done here — it
 * happens when the writer's own full request arrives (head) or when the
 * generation becomes head via arts_progress_frontier.
 */
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
      db_list->head = db_list->tail = arts_new_db_frontier();
      arts_writer_unlock(&db_list->writer);
    }
  }

  arts_reader_lock(&db_list->reader, &db_list->writer);
  bool registered = false;
  bool landed_on_head = false;
  bool is_head = true;
  struct arts_db_frontier_s *stamped = NULL;
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    if (frontier_add_write_lock(&frontier->lock)) {
      /*
       * An EW writer must own a DEDICATED generation. If the frontier we just
       * write-locked already holds entries (a prior generation's RO readers
       * that landed here before us in CDAG order), stamping the writer here
       * would fuse the reader and the writer into one generation: the reader
       * would then read live DB memory that the co-located writer is about to
       * overwrite (or has already overwritten via its async update), which is
       * exactly the t-1 / t+1 flake across multiple generations. Release the
       * lock unsealed-for-reuse is impossible (WRITE_SET is sticky), so we
       * instead seal this populated frontier as a read-only generation by
       * leaving it without a writer and advance to a fresh generation. The
       * seal harmlessly forces later acquires past it; the readers already on
       * it are satisfied when it reaches head.
       */
      if (frontier->position != 0) {
        frontier_unlock(&frontier->lock);
        is_head = false;
        if (!frontier->next) {
          struct arts_db_frontier_s *new_frontier = arts_new_db_frontier();
          if (arts_atomic_cswap_ptr((volatile void **)&frontier->next, NULL,
                                    new_frontier)) {
            arts_delete_db_frontier(new_frontier);
            while (!frontier->next) {
              ARTS_SPIN_PAUSE();
            }
          }
        }
        continue;
      }
      bool inserted =
          arts_push_db_to_element(&frontier->list, frontier->position, rank);
      if (inserted) {
        frontier->position++;
      }
      frontier->exNode = rank;
      frontier->exEdtGuid = edt_guid;
      frontier->exEdt = NULL;
      frontier->exSlot = slot;
      frontier->exMode = mode;
      frontier->exPreRegistered = true;
      frontier->exDelivered = false;
      frontier_unlock(&frontier->lock);
      registered = true;
      landed_on_head = is_head;
      stamped = frontier;
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
      }
    }
  }
  arts_reader_unlock(&db_list->reader);
  (void)stamped;
  (void)landed_on_head;
  return registered;
}

/*
 * Reserve a dedicated generation for a LOCAL EW writer in CDAG order. Unlike a
 * remote writer (served via the exclusive-writer slot at the head signal), a
 * local writer is satisfied through the normal acquire path: it later joins
 * THIS reserved generation by matching localWriteEdtGuid
 * (arts_try_join_same_local_writer), runs, and progresses the frontier on
 * release. Sealing the generation (WRITE_SET via frontier_add_write_lock, kept
 * by the sticky frontier_unlock) makes any subsequent reader/writer land on a
 * strictly-later generation, fixing the local-writer -> remote-reader order.
 */
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
      db_list->head = db_list->tail = arts_new_db_frontier();
      arts_writer_unlock(&db_list->writer);
    }
  }
  /*
   * Fully serialize per DB under the writer lock (see the reader prereg for
   * rationale: lock-free walks scramble CDAG order under concurrent prereg).
   * A writer always gets its OWN dedicated generation. If the tail already
   * holds a writer or readers, append a fresh generation; otherwise reuse the
   * empty tail. Seal the chosen generation WRITE_SET so later acquires/readers
   * land strictly after it, and stamp localWriteEdtGuid so the writer's own
   * acquire joins it (arts_try_join_same_local_writer) and drives it normally.
   */
  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_new_db_frontier();
  }
  struct arts_db_frontier_s *frontier = db_list->head;
  while (frontier->next) {
    frontier = frontier->next;
  }
  struct arts_db_frontier_s *target = frontier;
  bool tail_used = (frontier->position != 0 || frontier->roReadersCount != 0 ||
                    frontier->localRoReadersPos != 0 ||
                    frontier->localWriteEdtGuid != NULL_GUID ||
                    frontier->exEdtGuid != NULL_GUID ||
                    (frontier->lock & WRITE_SET) != 0);
  if (tail_used) {
    frontier->next = arts_new_db_frontier();
    target = frontier->next;
  }
  target->localWriteEdtGuid = edt_guid;
  target->localWriteEdt = NULL;
  /* Seal WRITE_SET so subsequent participants land on a later generation. */
  arts_atomic_fetch_or(&target->lock, WRITE_SET);
  arts_writer_unlock(&db_list->writer);
  return true;
}

/*
 * Reserve a generation for an owner-LOCAL RO reader in CDAG order (single-node).
 * The reader is delivered IN-PLACE later (it joins this reserved generation in
 * arts_claim_local_ro_reader when its acquire runs), so unlike a remote reader
 * it is recorded on localRoReaders (not roReaders) and is NOT copy-served. The
 * reservation marks the generation non-empty (localRoPending) so a following EW
 * writer opens a strictly-later sealed generation.
 */
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
      db_list->head = db_list->tail = arts_new_db_frontier();
      arts_writer_unlock(&db_list->writer);
    }
  }
  /*
   * Serialize per DB under the writer lock (same rationale as the remote reader
   * prereg): the tail must be stable so the reader lands after the latest
   * writer, preserving CDAG order under concurrent prereg.
   */
  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_new_db_frontier();
  }
  struct arts_db_frontier_s *frontier = db_list->head;
  while (frontier->next) {
    frontier = frontier->next;
  }
  bool tail_is_writer = (frontier->exEdt != NULL ||
                         frontier->exEdtGuid != NULL_GUID ||
                         frontier->localWriteEdtGuid != NULL_GUID ||
                         (frontier->lock & WRITE_SET) != 0 ||
                         frontier->position != 0);
  struct arts_db_frontier_s *target = frontier;
  if (tail_is_writer) {
    /* Open a fresh reader generation after the latest writer. */
    frontier->next = arts_new_db_frontier();
    target = frontier->next;
  }
  arts_push_ro_reader(&target->localRoReaders, target->localRoReadersPos,
                      arts_global_rank_id, edt_guid, slot);
  target->localRoReadersPos++;
  target->localRoPending++;
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
      if (cur->edt_guid[pos] == edt_guid) {
        /* Consume the reservation so a later duplicate dep cannot re-join. */
        cur->edt_guid[pos] = NULL_GUID;
        frontier->localRoPending--;
        frontier_lock(&frontier->lock);
        arts_atomic_add(&frontier->roOutstanding, 1U);
        bool head_match = (frontier == db_list->head);
        if (!head_match) {
          /* Delivered in-place when this generation is promoted to head. */
          arts_push_delayed_edt(&frontier->localDelayed,
                                frontier->localPosition++, edt, slot, mode);
        }
        frontier_unlock(&frontier->lock);
        if (head_match && on_head) {
          *on_head = true;
        }
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
                                 arts_guid_t edt_guid, bool *on_head,
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
    if (frontier->exPreRegistered && frontier->exNode == rank &&
        frontier->exEdtGuid == edt_guid) {
      found = true;
      if (on_head) {
        *on_head = is_head;
      }
      /*
       * Deliver exactly once, and only when this writer's generation is the
       * current head. The head writer has no predecessor whose retirement would
       * promote it, so its own late full request — which, by construction,
       * arrives only after the writer EDT exists and has run acquire — is the
       * naturally CDAG-ordered trigger that ships it. A non-head writer is NOT
       * shipped here; arts_progress_frontier ships it when its generation
       * reaches head, so it never observes a pre-predecessor DB value. The
       * exDelivered guard (also honored by arts_signal_frontier_local/remote)
       * keeps delivery to exactly one of the two paths.
       */
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

/*
 * Pre-register a remote RO reader in CDAG order (dual of the EW writer prereg).
 * The reader must land on a *reader* generation: never fused with an exclusive
 * writer (it would read memory the writer is about to overwrite) and never
 * before an earlier writer. Walk from head: skip any exclusive-writer
 * generation (read-lock fails on it — it is sealed/owned — or it is already
 * stamped as a writer), and join the first reader generation, creating a fresh
 * one at the tail if needed. Reader generations accept many readers, so we hold
 * a read-lock (not a write-lock) on the chosen generation.
 */
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
      db_list->head = db_list->tail = arts_new_db_frontier();
      arts_writer_unlock(&db_list->writer);
    }
  }

  /*
   * Pre-registration MUST be fully serialized per DB and atomic with respect to
   * the structure of the frontier list: take the db_list WRITER lock (exclusive
   * against other preregs, acquires, and progress) so the list is stable while
   * we inspect the tail and append. A lock-free tail walk is not safe here — a
   * concurrent prereg's freshly CAS-linked generation may not yet be visible,
   * causing this reader to mis-land (join an earlier reader generation instead
   * of opening one after the latest writer), which scrambles CDAG order.
   */
  arts_writer_lock(&db_list->reader, &db_list->writer);
  if (!db_list->head) {
    db_list->head = db_list->tail = arts_new_db_frontier();
  }
  bool registered = false;
  bool landed_on_head = false;
  struct arts_db_frontier_s *frontier = db_list->head;
  unsigned int depth = 0;
  while (frontier->next) {
    frontier = frontier->next;
    depth++;
  }
  /* frontier is the tail (stable under the writer lock). */
  bool tail_is_writer = (frontier->exEdt != NULL ||
                         frontier->exEdtGuid != NULL_GUID ||
                         (frontier->lock & WRITE_SET) != 0 ||
                         frontier->position != 0);
  struct arts_db_frontier_s *target = frontier;
  if (tail_is_writer) {
    /* Open a fresh reader generation after the latest writer. */
    frontier->next = arts_new_db_frontier();
    target = frontier->next;
  }
  arts_push_ro_reader(&target->roReaders, target->roReadersCount, rank, edt_guid,
                      slot);
  target->roReadersCount++;
  registered = true;
  /*
   * Serve immediately iff this reader's generation is currently the HEAD (no
   * earlier pending writer). Determined by direct identity against db_list->head
   * under the writer lock, which is exact: it covers both "the frontier was
   * already drained to head before this prereg" (the stranded-last-reader race,
   * where the producing writer progressed past an empty frontier before the
   * reader registered) and "this is the first generation". A head reader
   * generation is never signaled by a predecessor, so this is its only serve
   * point; a non-head reader is served by its writer's promotion. roReadersServed
   * keeps it exactly-once across the two paths.
   */
  landed_on_head = (target == db_list->head);
  if (landed_on_head) {
    frontier_lock(&target->lock);
    arts_serve_ro_readers(target, db);
    frontier_unlock(&target->lock);
    /*
     * A head reader generation served here has no writer to retire it. If it is
     * fully drained (all its readers served, none owner-local outstanding), pop
     * it and promote the following generation (the next writer / reader), so the
     * chain does not stall behind a permanently-head served reader generation.
     * Done under the writer lock we already hold; deferred deletion after
     * unlock. Loop to drain a run of consecutive served reader generations.
     */
    while (db_list->head && arts_reader_gen_drained(db_list->head)) {
      struct arts_db_frontier_s *drained = db_list->head;
      db_list->head = db_list->head->next;
      drained->next = NULL;
      if (db_list->head) {
        /* This frontier is owner-local (we are the owner registering a reader);
         * promote the next generation with the local head signal. */
        arts_signal_frontier_local(db_list->head, db);
      }
      arts_delete_db_frontier(drained);
    }
  }
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
  bool found = false;
  for (struct arts_db_frontier_s *frontier = db_list->head;
       frontier && !found; frontier = frontier->next) {
    frontier_lock(&frontier->lock);
    /*
     * Match served OR unserved generations: a remote RO reader served by a
     * targeted snapshot must NEVER also be shipped the DB via the legacy
     * node-granular send_now (double delivery into the EDT slot / dangling OO).
     * The reader's aggregated request carries no edt_guid, so with edt_guid ==
     * NULL_GUID we match any reader from this node; otherwise match exactly.
     */
    if (frontier->roReadersCount) {
      unsigned int n = frontier->roReadersCount;
      struct arts_ro_reader_s *cur = &frontier->roReaders;
      for (unsigned int i = 0; i < n; i++) {
        unsigned int pos = i % DBSPERELEMENT;
        if (cur->node[pos] == rank &&
            (edt_guid == NULL_GUID || cur->edt_guid[pos] == edt_guid)) {
          found = true;
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
    frontier_unlock(&frontier->lock);
  }
  arts_reader_unlock(&db_list->reader);
  return found;
}

/*
 * Serve every pre-registered remote RO reader on this (now-head) generation a
 * targeted snapshot of the current owner DB. Called from the frontier head
 * signal under the frontier lock. Exactly-once via roReadersServed.
 */
static void arts_serve_ro_readers(struct arts_db_frontier_s *frontier,
                                  struct arts_db_s *db) {
  if (!frontier->roReadersCount || frontier->roReadersServed) {
    return;
  }
  frontier->roReadersServed = true;
  unsigned int payload =
      (unsigned int)(db->header.size - sizeof(struct arts_db_s));
  void *src = (void *)(db + 1);
  unsigned int n = frontier->roReadersCount;
  struct arts_ro_reader_s *cur = &frontier->roReaders;
  for (unsigned int i = 0; i < n; i++) {
    unsigned int pos = i % DBSPERELEMENT;
    unsigned int node = cur->node[pos];
    arts_guid_t edt_guid = cur->edt_guid[pos];
    unsigned int slot = cur->slot[pos];
    if (node == arts_global_rank_id) {
      /* Owner-local pre-registered reader: deliver a private copy directly. */
      void *copy = arts_malloc(payload ? payload : 1U);
      if (payload) {
        memcpy(copy, src, payload);
      }
      arts_signal_edt_ptr_with_guid(edt_guid, slot, db->guid, copy, payload);
      arts_free(copy);
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
      db_list->head = db_list->tail = arts_new_db_frontier();
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
      arts_remote_db_full_request(db->guid, (int)get_from, edt_guid,
                                  (int)frontier->exSlot, frontier->exMode);
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
        /* Acquire a route table ref for this dep slot — matched by
         * return_db in release_dbs after EDT execution. */
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

  /*
   * PURE-RO head: no local or remote exclusive writer on this frontier. Remote
   * RO readers are served by the unified per-generation targeted snapshot path
   * (arts_serve_ro_readers from the roReaders list); roOutstanding for
   * owner-local readers is seeded once per reader at registration
   * (arts_push_db_to_list) and retires the head on the last release. EW-driven
   * frontiers (single-pass, matmul, 1-node) carry no RO consumers and retire on
   * their write-release path, so they are unaffected by the accounting below.
   *
   * The legacy element-iterated untargeted push and the promotion-time
   * roOutstanding seed are intentionally NOT done here: they double-delivered
   * remote readers and double-seeded roOutstanding, racing the W->R->W
   * ping-pong.
   */
  if (frontier->localPosition) {
    struct arts_local_delayed_edt_s *current = &frontier->localDelayed;
    for (unsigned int i = 0; i < frontier->localPosition; i++) {
      unsigned int pos = i % DBSPERELEMENT;
      struct arts_edt_s *edt = current->edt[pos];
      // TODO(gpu): GPU EDTs need GPU memory, not this CPU pointer.
      arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
      /* Acquire a route table ref for this dep slot. */
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
  /* Targeted per-generation snapshot for pre-registered remote RO readers. */
  arts_serve_ro_readers(frontier, db);
  frontier_unlock(&frontier->lock);
}

/*
 * A pure reader generation (no exclusive writer, no owner-local outstanding
 * readers) whose pre-registered remote RO readers have all been served has no
 * remaining consumer to retire it: those readers were satisfied by a targeted
 * snapshot and will never call back into the owner frontier. Such a generation
 * must self-retire so the following writer generation is promoted. Returns true
 * if the head is exactly this kind of drained reader generation.
 */
static bool arts_reader_gen_drained(struct arts_db_frontier_s *frontier) {
  if (!frontier) {
    return false;
  }
  bool drained = false;
  frontier_lock(&frontier->lock);
  bool reader_gen =
      (frontier->exEdt == NULL && frontier->exEdtGuid == NULL_GUID);
  if (reader_gen && frontier->roReadersCount > 0 &&
      frontier->roReadersServed && frontier->roOutstanding == 0) {
    drained = true;
  }
  frontier_unlock(&frontier->lock);
  return drained;
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
  struct arts_db_frontier_s *tail = db_list->head;
  struct arts_db_frontier_s *to_delete_head = NULL;
  struct arts_db_frontier_s *to_delete_tail = NULL;
  if (db_list->head) {
    db_list->head = db_list->head->next;
    while (db_list->head) {
      if (rank == arts_global_rank_id) {
        arts_signal_frontier_local(db_list->head, db);
      } else {
        arts_signal_frontier_remote(db_list->head, db, rank);
      }
      if (!arts_reader_gen_drained(db_list->head)) {
        break;
      }
      /* Drained reader generation: pop and defer its deletion. */
      struct arts_db_frontier_s *drained = db_list->head;
      db_list->head = db_list->head->next;
      drained->next = NULL;
      if (!to_delete_head) {
        to_delete_head = to_delete_tail = drained;
      } else {
        to_delete_tail->next = drained;
        to_delete_tail = drained;
      }
    }
  }
  arts_writer_unlock(&db_list->writer);
  // This should be safe since the writer lock ensures all readers are done
  if (tail) {
    arts_delete_db_frontier(tail);
  }
  while (to_delete_head) {
    struct arts_db_frontier_s *next = to_delete_head->next;
    arts_delete_db_frontier(to_delete_head);
    to_delete_head = next;
  }
}

bool arts_progress_and_get_frontier(struct arts_db_list_s *db_list,
                                    struct arts_db_frontier_iterator_s *iter) {
  arts_writer_lock(&db_list->reader, &db_list->writer);
  struct arts_db_frontier_s *tail = db_list->head;
  db_list->head = db_list->head->next;
  arts_writer_unlock(&db_list->writer);
  // This should be safe since the writer lock ensures all readers are done
  return arts_db_frontier_iter_init(iter, tail);
}
