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
/* route_table_iter — the iterator walks every published slot, and the one
 * install rule holds for a caller-made control block: an occupied slot is
 * never replaced.  The objects are static sentinels installed with no deleter,
 * so no retire frees them. */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_state.h"
#include "arts/utils/shared.h"
#include "../test_failure_status.h"

#define MYSIZE 10

static int g_obj[MYSIZE];
static int g_other;

static bool install_sentinel(void *obj, arts_guid_t key) {
  arts_shared_ptr_t cb = arts_shared_make(obj, NULL);
  arts_shared_set_tag(cb, (uint64_t)key);
  if (arts_route_table_install_handle_if_absent(cb, key)) {
    return true;
  }
  arts_shared_abandon(&cb);
  return false;
}

static unsigned int count_sentinels(arts_route_table_t *table) {
  unsigned int n = 0;
  arts_route_table_iterator_t iter;
  arts_reset_route_table_iterator(&iter, table);
  for (arts_route_item_t *item = arts_route_table_iterate(&iter); item != NULL;
       item = arts_route_table_iterate(&iter)) {
    arts_shared_ptr_t h = arts_route_table_lookup(
        __atomic_load_n(&item->key, __ATOMIC_ACQUIRE));
    void *obj = h ? arts_shared_get(h) : NULL;
    if (obj >= (void *)&g_obj[0] && obj < (void *)&g_obj[MYSIZE]) {
      n++;
    }
    arts_shared_release(&h);
  }
  return n;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t range =
      arts_guid_reserve_range(ARTS_GUID_EDT, MYSIZE, arts_get_current_rank());
  for (unsigned int i = 0; i < MYSIZE; i++) {
    if (!install_sentinel(&g_obj[i], arts_guid_from_index(range, i))) {
      arts_printf("FAIL: route_table_iter install into an empty slot %u lost\n",
                  i);
      arts_test_fail();
    }
  }

  unsigned int seen = 0;
  for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
    seen += count_sentinels(arts_node_info.route_table[t]);
  }
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    seen += count_sentinels(arts_node_info.remote_route_table[s]);
  }
  if (seen != MYSIZE) {
    arts_printf("FAIL: route_table_iter walked %u installed slots, want %u\n",
                seen, MYSIZE);
    arts_test_fail();
  }

  arts_guid_t first = arts_guid_from_index(range, 0);
  if (install_sentinel(&g_other, first)) {
    arts_printf("FAIL: route_table_iter an install replaced a live object\n");
    arts_test_fail();
  }
  arts_shared_ptr_t h = arts_route_table_lookup(first);
  if (h == NULL || arts_shared_get(h) != (void *)&g_obj[0]) {
    arts_printf("FAIL: route_table_iter the live object left its slot\n");
    arts_test_fail();
  }
  arts_shared_release(&h);

  for (unsigned int i = 0; i < MYSIZE; i++) {
    if (!arts_route_table_set_destroyed_object(arts_guid_from_index(range, i),
                                              &g_obj[i])) {
      arts_printf("FAIL: route_table_iter retire of slot %u failed\n", i);
      arts_test_fail();
    }
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS: route_table_iter %u slots walked, no replace\n", MYSIZE);
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
