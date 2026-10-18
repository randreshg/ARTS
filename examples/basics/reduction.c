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

#include <inttypes.h>

#include "arts.h"

unsigned int num_dbs = 0;
arts_guid_t reduction_guid = NULL_GUID;

void reduction(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t total = 0;
  for (unsigned int i = 0; i < depc; i++) {
    int *db_ptr = depv[i].ptr;
    total += db_ptr[0];
    arts_db_destroy(depv[i].guid);
  }
  arts_edt_satisfy_slot((arts_guid_t)paramv[0], 0, (arts_guid_t)total,
                        DB_MODE_NULL);
}

void shut_down(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  (void)paramv;
  arts_printf("Result %" PRIdPTR "\n", depv[0].guid);
  arts_shutdown();
}

void node_setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int node_id = (unsigned int)paramv[0];
  int *ptr;
  arts_guid_t db_guid = arts_db_create((void **)&ptr, sizeof(unsigned int),
                                       ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  *ptr = (int)node_id;
  arts_db_release(db_guid, DB_MODE_RW);
  arts_add_dependence(db_guid, (arts_guid_t)paramv[1], node_id, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  num_dbs = arts_get_total_ranks();
  reduction_guid = arts_guid_reserve(ARTS_GUID_EDT, 0);

  arts_guid_t guid =
      arts_edt_create(shut_down, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_edt_create(reduction, 1, (uint64_t *)&guid, num_dbs,
                  &(arts_edt_hint_t){.guid = reduction_guid});

  for (unsigned int n = 0; n < num_dbs; n++) {
    uint64_t args[2] = {n, (uint64_t)reduction_guid};
    arts_edt_create(node_setup, 2, args, 0, &(arts_edt_hint_t){.rank = n});
  }
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
