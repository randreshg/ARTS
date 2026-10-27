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
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "arts.h"
#include "arts/graph.h"

// This rank's copies of main_edt's distribution and max_reducer's GUID,
// written only by the rank's init_node; every reader runs after the loaders'
// finish scope.
arts_block_dist_t distribution;
char *file = NULL;
arts_guid_t max_reducer_guid = NULL_GUID;

uint64_t start_time;
uint64_t end_time;

typedef struct {
  arts_guid_t find_intersection_guid;
  arts_vertex_t source;
  unsigned int num_neighbors;
  arts_vertex_t neighbors[];
} source_info_t;

typedef struct {
  arts_vertex_t source;
  uint64_t scan_stat;
} per_vertex_scan_stat_t;

// int compare(const void * a, const void * b)
// {
//   return ( *(uint64_t*)a - *(uint64_t*)b );
// }

// arts_guid_t visit_source(uint32_t paramc, uint64_t * paramv, uint32_t depc,
// arts_edt_dep_t depv[]);

// arts_guid_t exit_program(uint32_t paramc, uint64_t * paramv, uint32_t depc,
// arts_edt_dep_t depv[]) {
//   arts_printf("Called exit\n");
//   arts_shutdown();
// }

void max_reducer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  // std::cout << "In max reducer" << std::endl;
  uint64_t max_scan_stat = 0;
  arts_vertex_t max_vertex = 0;
  for (uint32_t v = 0; v < depc; v++) {
    per_vertex_scan_stat_t *vertex_scan_stat =
        (per_vertex_scan_stat_t *)depv[v].ptr;
    // std::cout << "Vertex: " << vertex_scan_stat->source << " scan_stat: " <<
    // vertex_scan_stat->scan_stat << std::endl;
    if (vertex_scan_stat->scan_stat > max_scan_stat) {
      max_scan_stat = vertex_scan_stat->scan_stat;
      max_vertex = vertex_scan_stat->source;
    }
    arts_db_destroy(depv[v].guid);
  }
  std::cout << "Max vertex_t: " << max_vertex << " scan_stat: " << max_scan_stat
            << '\n';
  end_time = arts_get_time_stamp();
  arts_printf("Total execution time: %f s \n",
              (double)(end_time - start_time) / 1000000000.0);
  for (arts_partition_t p = 0; p < distribution.num_blocks; ++p) {
    arts_db_destroy(arts_block_dist_guid_for_partition(&distribution, p));
  }
  arts_shutdown();
}

// paramv[0]: the source's degree.  Slot r: the count from rank r.
void find_intersection(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t sum = 0;
  per_vertex_scan_stat_t *local_intersection =
      (per_vertex_scan_stat_t *)depv[0].ptr;
  arts_vertex_t source = local_intersection->source;

  for (uint64_t rank = 0; rank < depc; rank++) {
    per_vertex_scan_stat_t *local_intersection =
        (per_vertex_scan_stat_t *)depv[rank].ptr;
    // std::cout << "Source: " << source << " Rank: " << rank << "Scanstat: " <<
    // local_intersection->scan_stat << std::endl;
    sum += local_intersection->scan_stat;
    arts_db_destroy(depv[rank].guid);
  }

  uint64_t neighbor_cnt = paramv[0];

  sum += neighbor_cnt;

  unsigned int db_size = sizeof(per_vertex_scan_stat_t);
  void *ptr = NULL;
  arts_guid_t db_guid =
      arts_db_create(&ptr, db_size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  per_vertex_scan_stat_t *vertex_scan_stat = (per_vertex_scan_stat_t *)ptr;
  vertex_scan_stat->source = source;
  vertex_scan_stat->scan_stat = sum;
  // std::cout << "Source " << source << " ScanStat: " << sum << std::endl;
  arts_db_release(db_guid, DB_MODE_RW);
  arts_add_dependence(db_guid, max_reducer_guid, source, DB_MODE_RO);
}

// Slot 0: the source's information block (its neighbour list and the
// find_intersection to report to), which this EDT destroys; slot 1: this
// rank's partition.
void visit_one_hop_neighbor_on_rank(uint32_t paramc, const uint64_t *paramv,
                                    uint32_t depc, arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  (void)paramv;
  source_info_t *src_info = (source_info_t *)depv[0].ptr;
  arts_csr_graph_t *graph = (arts_csr_graph_t *)depv[1].ptr;
  arts_vertex_t *one_hop_neighbor;
  arts_vertex_t *immediate_neighbors = src_info->neighbors;
  std::vector<arts_vertex_t> local_intersection;
  for (unsigned int i = 0; i < src_info->num_neighbors; i++) {
    arts_vertex_t current_neighbor = src_info->neighbors[i];
    if (arts_block_dist_get_owner(current_neighbor, &distribution) ==
        arts_get_current_rank()) {
      // std::cout << "Source " << src_info->source << " Current_neighbor: " <<
      // current_neighbor << std::endl;
      arts_vertex_t *one_hop_neighbors = NULL;
      uint64_t neighbor_cnt = 0;
      arts_csr_get_neighbors(graph, current_neighbor, &one_hop_neighbors,
                             &neighbor_cnt);
      for (unsigned int j = 0; j < neighbor_cnt; j++) {
        // std::cout << "One-hop neighbor for " <<  src_info->source << " is: "
        // << one_hop_neighbors[j] << std::endl;
      }
      std::set_intersection(immediate_neighbors,
                            immediate_neighbors + src_info->num_neighbors,
                            one_hop_neighbors, one_hop_neighbors + neighbor_cnt,
                            std::back_inserter(local_intersection));
    }
  }
  arts_db_release(depv[1].guid, DB_MODE_RO);

  unsigned int db_size = sizeof(per_vertex_scan_stat_t);
  void *ptr = NULL;
  arts_guid_t db_guid =
      arts_db_create(&ptr, db_size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  per_vertex_scan_stat_t *vertex_scan_stat = (per_vertex_scan_stat_t *)ptr;
  vertex_scan_stat->source = src_info->source;
  vertex_scan_stat->scan_stat = local_intersection.size();
  // std::cout << "Source: " << src_info->source << " rank: "  <<
  // arts_get_current_node() << " set intersection size: " <<
  // local_intersection.size() <<std::endl;
  arts_db_release(db_guid, DB_MODE_RW);
  arts_add_dependence(db_guid, src_info->find_intersection_guid,
                      arts_get_current_rank(), DB_MODE_RO);
  arts_db_destroy(depv[0].guid);
}

// paramv[0]: the source.  Slot 0: a start signal; slot 1: the source's
// partition.
void visit_source(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  arts_csr_graph_t *graph = (arts_csr_graph_t *)depv[1].ptr;
  arts_guid_t partition_guid = depv[1].guid;
  arts_vertex_t *neighbors = NULL;
  uint64_t neighbor_cnt = 0;
  arts_vertex_t source = (arts_vertex_t)paramv[0];
  // std::cout << "Visiting source: " << source <<std::endl;
  arts_csr_get_neighbors(graph, source, &neighbors, &neighbor_cnt);
  if (neighbor_cnt) {
    /*Now spawn an edt that will wait to get oneHopneighbors from all the ranks
     * in slots and calculate the grand count */
    arts_edt_hint_t local_hint = {};
    local_hint.rank = arts_get_current_rank();
    arts_guid_t find_intersection_guid =
        arts_edt_create(find_intersection, 1, &neighbor_cnt,
                        arts_get_total_ranks(), &local_hint);
    /*For each rank, now spawn an edt that will perform an intersection*/
    for (unsigned int i = 0; i < arts_get_total_ranks(); i++) {
      unsigned int db_size =
          sizeof(source_info_t) + (sizeof(arts_vertex_t) * neighbor_cnt);
      void *ptr = NULL;
      arts_guid_t db_guid =
          arts_db_create(&ptr, db_size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
      source_info_t *src_info = (source_info_t *)ptr;
      src_info->find_intersection_guid = find_intersection_guid;
      src_info->source = source;
      src_info->num_neighbors = neighbor_cnt;
      memcpy(&(src_info->neighbors), neighbors,
             sizeof(arts_vertex_t) * neighbor_cnt);
      arts_db_release(db_guid, DB_MODE_RW);
      // max_reducer destroys the partition, so it is released after the
      // row's last copy and before the last hand-off on the way there.
      if (i + 1 == arts_get_total_ranks()) {
        arts_db_release(partition_guid, DB_MODE_RO);
      }
      /*create the edt to find # one-hop neighbors*/
      arts_edt_hint_t hop_hint = {};
      hop_hint.rank = i;
      arts_guid_t visit_one_hop_neighbor_guid = arts_edt_create(
          visit_one_hop_neighbor_on_rank, 0, NULL, 2, &hop_hint);
      arts_add_dependence(db_guid, visit_one_hop_neighbor_guid, 0, DB_MODE_RO);
      arts_add_dependence(arts_block_dist_guid_for_partition(&distribution, i),
                          visit_one_hop_neighbor_guid, 1, DB_MODE_RO);
    }
  } else {
    arts_db_release(partition_guid, DB_MODE_RO);
    /*signal maxreducer*/
    unsigned int db_size = sizeof(per_vertex_scan_stat_t);
    void *ptr = NULL;
    arts_guid_t db_guid =
        arts_db_create(&ptr, db_size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    per_vertex_scan_stat_t *vertex_scan_stat = (per_vertex_scan_stat_t *)ptr;
    vertex_scan_stat->source = source;
    vertex_scan_stat->scan_stat = 1;
    // std::cout << "signaling maxruducer for source " << source << std::endl;
    arts_db_release(db_guid, DB_MODE_RW);
    arts_add_dependence(db_guid, max_reducer_guid, source, DB_MODE_RO);
  }
}

// paramv: main_edt's distribution (ARTS_BLOCK_DIST_PARAMC words), max_reducer's
// GUID, then main_edt's arguments as an argument block.
extern "C" void init_node(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  unsigned int node_id = arts_get_current_rank();

  distribution = arts_block_dist_from_params(paramv);
  max_reducer_guid = (arts_guid_t)paramv[ARTS_BLOCK_DIST_PARAMC];
  const uint64_t *arg_words = paramv + ARTS_BLOCK_DIST_PARAMC + 1;
  uint32_t arg_paramc = paramc - (ARTS_BLOCK_DIST_PARAMC + 1);
  int rc = -1;
  int argc = arts_argv_count(arg_words, arg_paramc);
  arts_printf("Node %u argc %d\n", node_id, argc);
  if (argc >= 0) {
    std::vector<const char *> argv((size_t)argc + 1);
    if (arts_argv_from_params(arg_words, arg_paramc, argv.data(), argc + 1) ==
        argc) {
      rc = arts_csr_load_from_args(&distribution, argc, argv.data());
    }
  }
  if (rc != 0) {
    arts_printf("Failed to load the graph on rank %u\n", node_id);
    arts_shutdown();
  }
}

// main_edt after every loader has finished, on rank 0, where max_reducer
// reads start_time.
void main_continue(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  // Create max reducer EDT
  arts_edt_hint_t reducer_hint = {};
  reducer_hint.guid = max_reducer_guid;
  arts_edt_create(max_reducer, 0, NULL, distribution.num_vertices,
                  &reducer_hint);
  start_time = arts_get_time_stamp();
  for (uint64_t i = 0; i < distribution.num_vertices; ++i) {
    uint64_t source = i;
    arts_partition_t rank = arts_block_dist_get_owner(source, &distribution);
    uint64_t packed_values[1] = {source};
    arts_edt_hint_t rank_hint = {};
    rank_hint.rank = rank;
    arts_guid_t visit_source_guid = arts_edt_create(
        visit_source, 1, (uint64_t *)&packed_values, 2, &rank_hint);
    arts_edt_satisfy_slot(visit_source_guid, 0, 0, DB_MODE_NULL);
    arts_add_dependence(arts_block_dist_guid_for_partition(&distribution, rank),
                        visit_source_guid, 1, DB_MODE_RO);
  }
}

extern "C" void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  const char *const *argv = (const char *const *)paramv[1];
  arts_block_dist_t dist;
  if (arts_block_dist_init_from_args(&dist, argc, argv) != 0) {
    arts_printf("Usage: %s --file <graph> --num-vertices <n> --num-edges <m> "
                "[--flip] [--keep-self-loops] [--csr-format]\n",
                argv[0]);
    arts_shutdown();
    return;
  }
  uint32_t arg_paramc = arts_argv_paramc(argc, argv);
  if (arg_paramc == 0) {
    arts_printf("%s: the arguments do not fit in EDT parameters\n", argv[0]);
    arts_shutdown();
    return;
  }
  arts_guid_t reducer_guid = arts_guid_reserve(ARTS_GUID_EDT, 0);

  // Initialize graph data on every node
  std::vector<uint64_t> init_paramv(ARTS_BLOCK_DIST_PARAMC + 1 + arg_paramc);
  arts_block_dist_to_params(&dist, init_paramv.data());
  init_paramv[ARTS_BLOCK_DIST_PARAMC] = reducer_guid;
  arts_argv_to_params(argc, argv,
                      init_paramv.data() + ARTS_BLOCK_DIST_PARAMC + 1);
  arts_event_hint_t finish_hint = {};
  finish_hint.rank = ARTS_HINT_CURRENT_RANK;
  finish_hint.finish = true;
  arts_guid_t init_done = arts_event_create(&finish_hint);
  for (unsigned int i = 0; i < arts_get_total_ranks(); i++) {
    arts_edt_hint_t node_hint = {};
    node_hint.rank = i;
    node_hint.finish_event = init_done;
    arts_edt_create(init_node, (uint32_t)init_paramv.size(), init_paramv.data(),
                    0, &node_hint);
  }
  arts_edt_hint_t cont_hint = {};
  cont_hint.rank = 0;
  arts_guid_t cont =
      arts_edt_create(main_continue, paramc, paramv, 1, &cont_hint);
  arts_add_dependence(init_done, cont, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
