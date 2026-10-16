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
#ifndef ARTS_GPU_GPUROUTETABLE_H
#define ARTS_GPU_GPUROUTETABLE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>

#include "arts/gas/route_table.h"
#include "arts/utils/stdatomic_compat.h"

typedef struct {
  /* Written by the slot's installer before real_data is published; read
   * only at teardown. */
  uint64_t size;
  /* Logical clock of the slot's last touch: the maximum of the tickets its
   * users drew.  It carries only its own value. */
  _Atomic(unsigned int) time_stamp;
  /* The mirrored item, written once per slot lifetime by its installer with
   * a release store after everything a user must see is submitted (for a
   * staged block, its host-to-device copy, enqueued on the device's stream or
   * placed in its submission buffer); every user loads it with acquire. */
  _Atomic(void *) real_data;
} arts_item_wrapper_t;

typedef struct {
  arts_route_table_t routingTable;
  arts_item_wrapper_t *wrappers;
  pthread_mutex_t gc_lock; /* one GC sweep of this table at a time */
} arts_gpu_route_table_t;

arts_route_table_t *arts_gpu_new_route_table(unsigned int route_table_size,
                                             unsigned int shift);

uint64_t arts_gpu_lookup_db(arts_guid_t key);
void *arts_gpu_route_table_add_item(void *item, uint64_t size, arts_guid_t key,
                                    unsigned int gpu_id);
arts_item_wrapper_t *
arts_gpu_route_table_reserve_item(bool *added, uint64_t size, arts_guid_t key,
                                  unsigned int gpu_id, bool add_to_use);
void *arts_gpu_route_table_add_item_to_delete(void *item, uint64_t size,
                                              arts_guid_t key,
                                              unsigned int gpu_id);
void *arts_gpu_route_table_lookup_db(arts_guid_t key, int gpu_id,
                                     unsigned int *touched,
                                     unsigned int *time_stamp);
void *arts_gpu_route_table_lookup_db_res(arts_guid_t key, int gpu_id,
                                         const unsigned int *touched,
                                         unsigned int *time_stamp, bool res);
bool arts_gpu_route_table_return_db(arts_guid_t key, bool mark_to_delete,
                                    unsigned int gpu_id);
bool arts_gpu_invalidate_route_tables(arts_guid_t key,
                                      unsigned int keep_on_this_gpu);
bool arts_gpu_invalidate_on_route_table(arts_guid_t key, unsigned int gpu_id);
uint64_t arts_gpu_clean_up_route_table(unsigned int size_to_clean,
                                       bool clean_zeros, unsigned int gpu_id);
uint64_t arts_gpu_free_all(unsigned int gpu_id);

void gpu_gc_read_lock();
void gpu_gc_read_unlock();
void gpu_gc_write_lock();
void gpu_gc_write_unlock();

#ifdef __cplusplus
}
#endif

#endif
