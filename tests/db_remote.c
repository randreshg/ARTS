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

/// @file db_remote.c
/// @brief Tests remote DB create ordering. Requires node_count > 1.
///        Covers creator-frontier release and put/get to a remote owner.

#include "arts.h"
#include <string.h>

#define DATA_SIZE 256

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

/// EDT: verify put/get round-trip to remote DB.
void check_remote_put_get(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int target_rank = (unsigned int)paramv[0];
  unsigned char *data = (unsigned char *)depv[0].ptr;
  bool ok = (data != NULL && arts_get_current_node() == target_rank);
  if (ok) {
    for (unsigned int i = 0; i < DATA_SIZE && ok; i++) {
      if (data[i] != (unsigned char)(i & 0xFF)) {
        ok = false;
      }
    }
  }
  if (ok) {
    arts_printf("  PASS: remote DB create/put/read ordering correct\n");
  } else {
    arts_printf("  FAIL: remote DB create/put/read ordering mismatch "
                "(node=%u expected_node=%u data=%p)\n",
                arts_get_current_node(), target_rank, data);
    arts_abort(1);
  }
}

/// EDT: a no-data remote create must release the creator-held frontier when
/// the creator EDT exits, otherwise this RO reader never runs.
void check_remote_create_release(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int target_rank = (unsigned int)paramv[0];
  bool ok = (depv[0].ptr != NULL && arts_get_current_node() == target_rank);
  if (ok) {
    arts_printf("  PASS: remote DB creator frontier release unblocked RO\n");
  } else {
    arts_printf("  FAIL: remote DB creator release mismatch "
                "(node=%u expected_node=%u data=%p)\n",
                arts_get_current_node(), target_rank, depv[0].ptr);
    arts_abort(1);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_remote (multi-node) ===\n");
  if (arts_get_total_nodes() < 2) {
    arts_printf("  SKIP: requires >= 2 nodes\n");
    arts_shutdown();
    return;
  }

  unsigned int target = 1; // Remote node.

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  // Remote create is followed by an immediate put and then an owner-local RO
  // dependence. This exercises the create/initialization ordering edge: the
  // reader must not observe the DB until the put has released the creator
  // write.
  void *tmp;
  arts_guid_t remote_db = arts_db_create(&tmp, DATA_SIZE, ARTS_DB_DEFAULT,
                                         &(arts_hint_t){.route = target});
  if (tmp != NULL) {
    arts_printf("  FAIL: remote arts_db_create returned local pointer\n");
    arts_abort(1);
  }
  unsigned char send_buf[DATA_SIZE];
  for (unsigned int i = 0; i < DATA_SIZE; i++) {
    send_buf[i] = (unsigned char)(i & 0xFF);
  }

  uint64_t params[1];
  params[0] = (uint64_t)target;
  arts_guid_t read_edt = arts_edt_create_with_epoch(
      check_remote_put_get, 1, params, 1, epoch,
      &(arts_hint_t){.route = target});
  arts_put_in_db(send_buf, NULL_GUID, remote_db, 0, 0, DATA_SIZE);
  arts_add_dependence(remote_db, read_edt, 0, DB_MODE_RO);

  void *release_tmp;
  arts_guid_t release_db =
      arts_db_create(&release_tmp, sizeof(uint64_t), ARTS_DB_DEFAULT,
                     &(arts_hint_t){.route = target});
  if (release_tmp != NULL) {
    arts_printf("  FAIL: remote release DB create returned local pointer\n");
    arts_abort(1);
  }
  arts_guid_t release_reader = arts_edt_create_with_epoch(
      check_remote_create_release, 1, params, 1, epoch,
      &(arts_hint_t){.route = target});
  arts_add_dependence(release_db, release_reader, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
