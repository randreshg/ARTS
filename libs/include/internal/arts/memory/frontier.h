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
#ifndef ARTS_MEMORY_FRONTIER_H
#define ARTS_MEMORY_FRONTIER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/runtime_types.h"

#define DBSPERELEMENT 8

struct arts_db_element_s {
  struct arts_db_element_s *next;
  unsigned int array[DBSPERELEMENT];
};

struct arts_local_delayed_edt_s {
  struct arts_local_delayed_edt_s *next;
  struct arts_edt_s *edt[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
  arts_db_access_mode_t mode[DBSPERELEMENT];
};

struct arts_delayed_slice_request_s {
  struct arts_delayed_slice_request_s *next;
  struct arts_edt_s *edt[DBSPERELEMENT];
  arts_guid_t edt_guid[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
  uint32_t flags[DBSPERELEMENT];
  uint64_t offset[DBSPERELEMENT];
  uint64_t size[DBSPERELEMENT];
};

struct arts_ro_reader_s {
  struct arts_ro_reader_s *next;
  unsigned int node[DBSPERELEMENT];
  arts_guid_t edt_guid[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
};

struct arts_db_frontier_s {
  struct arts_db_element_s list;
  unsigned int position;
  struct arts_db_frontier_s *next;
  volatile unsigned int lock;

  unsigned int exNode;
  arts_guid_t exEdtGuid;
  struct arts_edt_s *exEdt;
  unsigned int exSlot;
  arts_db_access_mode_t exMode;

  bool exPreRegistered;
  bool exDelivered;
  bool exUpdatePending;
  uint64_t exUpdateSize;
  void *exUpdatePayload;

  bool headSignaled;

  unsigned int roReadersCount;
  bool roReadersServed;
  struct arts_ro_reader_s roReaders;

  unsigned int localRoReadersPos;
  unsigned int localRoPending;
  struct arts_ro_reader_s localRoReaders;

  arts_guid_t localWriteEdtGuid;
  struct arts_edt_s *localWriteEdt;

  unsigned int localPosition;
  struct arts_local_delayed_edt_s localDelayed;

  volatile unsigned int roOutstanding;

  unsigned int slicePosition;
  struct arts_delayed_slice_request_s sliceDelayed;
};

struct arts_db_list_s {
  struct arts_db_frontier_s *head;
  struct arts_db_frontier_s *tail;
  struct arts_db_frontier_s *free_frontiers;
  unsigned int free_frontiers_count;
  volatile unsigned int reader;
  volatile unsigned int writer;
};

struct arts_db_frontier_iterator_s {
  struct arts_db_frontier_s *frontier;
  unsigned int currentIndex;
  struct arts_db_element_s *currentElement;
};

struct arts_db_frontier_s *arts_new_db_frontier();
struct arts_db_list_s *arts_new_db_list();
void arts_delete_db_list(struct arts_db_list_s *db_list);
unsigned int arts_current_frontier_size(struct arts_db_list_s *db_list);
bool arts_db_frontier_iter_init(struct arts_db_frontier_iterator_s *iter,
                                struct arts_db_frontier_s *frontier);
unsigned int
arts_db_frontier_iter_size(struct arts_db_frontier_iterator_s *iter);
bool arts_db_frontier_iter_next(struct arts_db_frontier_iterator_s *iter,
                                unsigned int *next);
bool arts_db_frontier_iter_has_next(struct arts_db_frontier_iterator_s *iter);
void arts_progress_frontier(struct arts_db_s *db, unsigned int rank);
bool arts_apply_remote_writer_update(struct arts_db_s *db, unsigned int rank,
                                     arts_guid_t edt_guid,
                                     const void *payload,
                                     uint64_t payload_size);
bool arts_apply_remote_writer_put(struct arts_db_s *db, unsigned int rank,
                                  arts_guid_t edt_guid, uint64_t offset,
                                  const void *payload, uint64_t payload_size);
bool arts_release_remote_writer(struct arts_db_s *db, unsigned int rank,
                                arts_guid_t edt_guid);
bool arts_progress_and_get_frontier(struct arts_db_list_s *db_list,
                                    struct arts_db_frontier_iterator_s *iter);
bool arts_push_db_to_list(struct arts_db_list_s *db_list, unsigned int data,
                          bool write, bool local, bool bypass,
                          struct arts_edt_s *edt, arts_guid_t edt_guid,
                          unsigned int slot, arts_db_access_mode_t mode,
                          bool *on_head);
bool arts_request_db_slice(struct arts_db_s *db, struct arts_edt_s *local_edt,
                           arts_guid_t edt_guid, unsigned int slot,
                           uint64_t offset, uint64_t size, uint32_t flags);
bool arts_close_frontier(struct arts_db_list_s *db_list,
                         struct arts_db_frontier_iterator_s *iter);

bool arts_register_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot,
                                    arts_db_access_mode_t mode);

bool arts_register_local_ew_writer(struct arts_db_s *db, arts_guid_t edt_guid);

bool arts_register_local_ro_reader(struct arts_db_s *db, arts_guid_t edt_guid,
                                   unsigned int slot);

bool arts_claim_local_ro_reader(struct arts_db_s *db, struct arts_edt_s *edt,
                                arts_guid_t edt_guid, unsigned int slot,
                                arts_db_access_mode_t mode, bool *on_head);

bool arts_claim_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                 arts_guid_t edt_guid, unsigned int slot,
                                 arts_db_access_mode_t mode, bool *on_head,
                                 bool *deliver);

void arts_retire_local_ro_reader(struct arts_db_s *db);

bool arts_register_remote_ro_reader(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot);

bool arts_remote_ro_reader_preregistered(struct arts_db_s *db,
                                         unsigned int rank,
                                         arts_guid_t edt_guid);
bool arts_remote_ro_reader_preregistered_exact(struct arts_db_s *db,
                                               unsigned int rank,
                                               arts_guid_t edt_guid,
                                               unsigned int slot);
#ifdef __cplusplus
}
#endif

#endif /* ARTSDBLIST_H */
