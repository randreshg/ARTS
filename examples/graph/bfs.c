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
#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts.h"
#include "arts/graph.h"

/* Built in init_per_node, identically on every rank, from the rank's own
 * arguments, which init_node reads again to load its partition. */
arts_block_dist_t *distribution;
int saved_argc;
char **saved_argv;

/* The levels of this rank's vertices: relax EDTs on the rank lower them
 * concurrently, each by CAS, and exit_program reads rank 0's once the search's
 * finish scope has closed. */
_Atomic(uint64_t) *level;

void bfs_output() {
  arts_printf("Printing vertex levels....\n");
  uint64_t num_local_vertices =
      arts_block_dist_block_size(arts_get_current_rank(), distribution);
  uint64_t i;
  for (i = 0; i < num_local_vertices; ++i) {
    arts_printf("Local vertex : %" PRIu64 ", Level : %" PRIu64 "\n", i,
                atomic_load_explicit(&level[i], memory_order_relaxed));
  }
}

void exit_program(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  (void)paramv;
  bfs_output();
  free((void *)level);
  for (arts_partition_t p = 0; p < distribution->num_blocks; ++p) {
    arts_db_destroy(arts_block_dist_guid_for_partition(distribution, p));
  }
  arts_shutdown();
}

void bfs_send(arts_vertex_t u, uint64_t ulevel);

/* Slot 0: the partition that holds v, RO. */
void relax(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_printf("calling relax\n");
  assert(paramc == 2);
  arts_vertex_t v = (arts_vertex_t)paramv[0];
  uint64_t vlevel = paramv[1];
  arts_csr_graph_t *graph = (arts_csr_graph_t *)depv[0].ptr;

  arts_local_index_t indexv = arts_block_dist_get_local_index(v, distribution);
  assert(indexv < graph->num_local_vertices);

  uint64_t oldlevel =
      atomic_load_explicit(&level[indexv], memory_order_relaxed);
  bool success = false;
  while (vlevel < oldlevel) {
    success = atomic_compare_exchange_strong_explicit(
        &level[indexv], &oldlevel, vlevel, memory_order_relaxed,
        memory_order_relaxed);
    oldlevel = atomic_load_explicit(&level[indexv], memory_order_relaxed);
  }

  if (success) {
    // notify neighbors
    // get neighbors
    arts_vertex_t *neighbors = NULL;
    uint64_t neighbor_cnt = 0;
    arts_csr_get_neighbors(graph, v, &neighbors, &neighbor_cnt);

    // iterate over neighbors
    uint64_t neigbrlevel =
        atomic_load_explicit(&level[indexv], memory_order_relaxed) + 1;
    for (uint64_t i = 0; i < neighbor_cnt; ++i) {
      arts_vertex_t u = neighbors[i];

      // route message
      arts_printf("sending u=%" PRIu64 ", level= %" PRIu64 "\n", u,
                  neigbrlevel);
      bfs_send(u, neigbrlevel);
    }
  }
}

void bfs_send(arts_vertex_t u, uint64_t ulevel) {
  arts_guid_t neighb_dbguid = arts_block_dist_guid_for_vertex(u, distribution);
  uint64_t send[2];
  send[0] = u;
  send[1] = ulevel;
  arts_guid_t relax_guid = arts_edt_create(
      relax, 2, send, 1,
      &(arts_edt_hint_t){.rank = arts_guid_get_rank(neighb_dbguid)});
  arts_add_dependence(neighb_dbguid, relax_guid, 0, DB_MODE_RO);
}

void kickoff_termination(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_printf("Kick off\n");
  arts_vertex_t source = (arts_vertex_t)paramv[0];
  bfs_send(source, 0);
}

void init_node(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int node_id = arts_get_current_rank();

  int rc = arts_csr_load_from_args(distribution, saved_argc, saved_argv);

  uint64_t num_local_vertices =
      arts_block_dist_block_size(node_id, distribution);
  level = (_Atomic(uint64_t) *)malloc(num_local_vertices * sizeof(*level));
  for (uint64_t i = 0; i < num_local_vertices; ++i) {
    atomic_init(&level[i], UINT64_MAX);
  }

  if (rc != 0) {
    arts_printf("Failed to load the graph on rank %u\n", node_id);
    arts_shutdown();
  }
}

/* main_edt after every loader has finished: on rank 0, with main_edt's
 * paramv. */
void main_continue(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];

  // Find the source vertex
  arts_vertex_t source = 0;
  for (int i = 0; i < argc; ++i) {
    if (strcmp("--source", argv[i]) == 0 && i + 1 < argc) {
      source = (uint64_t)strtoull(argv[i + 1], NULL, 10);
    }
  }

  if (source >= distribution->num_vertices) {
    arts_printf("Usage: %s --file <graph> --num-vertices <n> --num-edges <m> "
                "[--source <vertex below n>] [--flip] [--keep-self-loops] "
                "[--csr-format]\n",
                argv[0]);
    arts_shutdown();
    return;
  }

  arts_guid_t exit_guid =
      arts_edt_create(exit_program, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_guid_t search_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(search_done, exit_guid, 0, DB_MODE_NULL);
  arts_edt_create(kickoff_termination, 1, (uint64_t *)&source, 0,
                  &(arts_edt_hint_t){.rank = 0, .finish_event = search_done});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  if (distribution == NULL) {
    arts_printf("Usage: %s --file <graph> --num-vertices <n> --num-edges <m> "
                "[--source <vertex below n>] [--flip] [--keep-self-loops] "
                "[--csr-format]\n",
                ((char **)paramv[1])[0]);
    arts_shutdown();
    return;
  }

  // Initialize graph data on every node
  arts_guid_t init_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int i = 0; i < arts_get_total_ranks(); i++) {
    arts_edt_create(init_node, 0, NULL, 0,
                    &(arts_edt_hint_t){.rank = i, .finish_event = init_done});
  }
  arts_guid_t cont = arts_edt_create(main_continue, paramc, paramv, 1,
                                     &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(init_done, cont, 0, DB_MODE_NULL);
}

void init_per_node(unsigned int node_id, int argc, char **argv) {
  (void)node_id;
  saved_argc = argc;
  saved_argv = argv;
  distribution = arts_block_dist_init_from_args(argc, argv);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
