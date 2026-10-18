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
#include <string.h>

#include "arts.h"
#include "arts/graph.h"

/* Built in init_per_node, identically on every rank, from the rank's own
 * arguments, which init_node reads again to load its partition; the reducers'
 * GUIDs are reserved there too, so every rank names the same EDTs. */
arts_block_dist_t *distribution;
int saved_argc;
char **saved_argv;

arts_guid_t start_reduce_guid = NULL_GUID;
arts_guid_t final_reduce_guid = NULL_GUID;

arts_vertex_t dist_start = 0;
/* One past this rank's last vertex. */
arts_vertex_t dist_end = 0;
uint64_t block_size = 0;
uint64_t over_sub = 16;

/* Added to by this rank's concurrent visit_vertex EDTs; local_reduce reads them
 * once the counting finish scope has closed. */
_Atomic(uint64_t) other_count = 0;
_Atomic(uint64_t) local_triangle_count = 0;
uint64_t timer = 0;

_Atomic(uint64_t) local = 0;
_Atomic(uint64_t) remote = 0;
_Atomic(uint64_t) incoming = 0;

void final_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]);
void local_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]);
void start_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]);
void visit_vertex(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]);

// Only support up to 64 nodes
static inline unsigned int check_and_set(uint64_t *mask, unsigned int index) {
  uint64_t bit = (uint64_t)1 << index;
  if (((*mask) & bit) == 0) {
    (*mask) |= bit;
    return 1;
  }
  return 0;
}

void final_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  uint64_t count = 0;
  for (unsigned int i = 0; i < depc; i++) {
    count += (uint64_t)depv[i].guid;
  }
  timer = arts_get_time_stamp() - timer;
  arts_printf("Triangle Count: %lu Time: %lu\n", count, timer);
  for (arts_partition_t p = 0; p < distribution->num_blocks; ++p) {
    arts_db_destroy(arts_block_dist_guid_for_partition(distribution, p));
  }
  arts_shutdown();
}

void local_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  (void)paramv;
  arts_printf("Local: %lu Remote: %lu Incoming: %lu\n",
              atomic_load_explicit(&local, memory_order_relaxed),
              atomic_load_explicit(&remote, memory_order_relaxed),
              atomic_load_explicit(&incoming, memory_order_relaxed));
  uint64_t count =
      atomic_load_explicit(&local_triangle_count, memory_order_relaxed) +
      atomic_load_explicit(&other_count, memory_order_relaxed);
  arts_edt_satisfy_slot(final_reduce_guid, arts_get_current_rank(),
                        (arts_guid_t)count, DB_MODE_NULL);
}

void start_reduce(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  (void)paramv;
  for (unsigned int i = 0; i < arts_get_total_ranks(); i++) {
    arts_edt_create(local_reduce, 0, NULL, 0, &(arts_edt_hint_t){.rank = i});
  }
}

static inline uint64_t lower_bound(arts_vertex_t value, uint64_t start,
                                   uint64_t end, const arts_vertex_t *edges) {
  while ((start < end) && (edges[start] < value)) {
    start++;
  }
  return start;
}

static inline uint64_t upper_bound(arts_vertex_t value, uint64_t start,
                                   uint64_t end, const arts_vertex_t *edges) {
  while ((start < end) && (value < edges[end - 1])) {
    end--;
  }
  return end;
}

static inline uint64_t count_triangles(const arts_vertex_t *a, uint64_t a_start,
                                       uint64_t a_end, const arts_vertex_t *b,
                                       uint64_t b_start, uint64_t b_end) {
  uint64_t count = 0;
  while ((a_start < a_end) && (b_start < b_end)) {
    if (a[a_start] < b[b_start]) {
      a_start++;
    } else if (a[a_start] > b[b_start]) {
      b_start++;
    } else {
      count++;
      a_start++;
      b_start++;
    }
  }
  return count;
}

/* graph is this rank's partition. */
static inline uint64_t process_vertex(arts_csr_graph_t *graph, arts_vertex_t i,
                                      arts_vertex_t *neighbors,
                                      uint64_t neighbor_count,
                                      uint64_t *visit_mask,
                                      uint64_t *proc_local,
                                      uint64_t *proc_remote) {
  uint64_t local_count = 0;

  uint64_t first_pred = lower_bound(i, 0, neighbor_count, neighbors);
  uint64_t last_pred = neighbor_count;
  //    arts_printf("%lu = %lu %lu\n", i, first_pred, last_pred);
  for (uint64_t next_pred = first_pred + 1; next_pred < last_pred;
       next_pred++) {
    arts_vertex_t j = neighbors[next_pred];
    unsigned int owner = arts_block_dist_get_owner(j, distribution);
    if (arts_block_dist_get_owner(j, distribution) == arts_get_current_rank()) {
      arts_vertex_t *j_neighbors = NULL;
      uint64_t j_neighbor_count = 0;
      arts_csr_get_neighbors(graph, j, &j_neighbors, &j_neighbor_count);
      uint64_t first_succ = lower_bound(i, 0, j_neighbor_count, j_neighbors);
      uint64_t last_succ = upper_bound(j, 0, j_neighbor_count, j_neighbors);
      uint64_t temp = count_triangles(neighbors, first_pred, next_pred,
                                      j_neighbors, first_succ, last_succ);
      local_count += temp;
      //            arts_printf("%lu %lu -- %lu\n", i, j, temp);
      (*proc_local)++;
    } else if (check_and_set(visit_mask, owner)) {
      uint64_t args[3];
      args[0] = i;
      args[1] = i;
      args[2] = neighbor_count;
      arts_guid_t guid = arts_edt_create(visit_vertex, 3, args, 2,
                                         &(arts_edt_hint_t){.rank = owner});
      arts_vertex_t *copy;
      arts_guid_t copy_guid =
          arts_db_create((void **)&copy, sizeof(arts_vertex_t) * neighbor_count,
                         ARTS_DB, ARTS_DB_PROP_NONE, NULL);
      memcpy(copy, neighbors, sizeof(arts_vertex_t) * neighbor_count);
      arts_db_release(copy_guid, DB_MODE_RW);
      arts_add_dependence(copy_guid, guid, 0, DB_MODE_RO);
      arts_add_dependence(
          arts_block_dist_guid_for_partition(distribution, owner), guid, 1,
          DB_MODE_RO);
      (*proc_remote)++;
    }
  }
  return local_count;
}

/* Two forms, told apart by their slot count.  Local (paramv: a vertex range;
 * slot 0: this rank's partition): the triangles whose smallest vertex lies in
 * the range.  Remote (paramv: one vertex twice and its degree; slot 0: a copy
 * of its neighbour list, which this EDT destroys, slot 1: this rank's
 * partition): that vertex's triangles through the neighbours this rank
 * owns. */
void visit_vertex(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t local_count = 0;

  arts_vertex_t start = paramv[0];
  arts_vertex_t end = paramv[1];

  arts_vertex_t *neighbors = NULL;
  uint64_t neighbor_count = 0;

  uint64_t proc_local = 0;
  uint64_t proc_remote = 0;
  uint64_t proc_incoming = 0;

  if (depc == 2) {
    arts_csr_graph_t *graph = (arts_csr_graph_t *)depv[1].ptr;
    neighbors = depv[0].ptr;
    neighbor_count = paramv[2];
    uint64_t visit_mask = (uint64_t)-1;
    local_count = process_vertex(graph, start, neighbors, neighbor_count,
                                 &visit_mask, &proc_incoming, &proc_remote);
    arts_db_destroy(depv[0].guid);
    atomic_fetch_add_explicit(&other_count, local_count, memory_order_relaxed);
    atomic_fetch_add_explicit(&incoming, proc_incoming, memory_order_relaxed);
  } else {
    arts_csr_graph_t *graph = (arts_csr_graph_t *)depv[0].ptr;
    for (arts_vertex_t i = start; i < end; i++) {
      uint64_t visit_mask = 0;
      arts_csr_get_neighbors(graph, i, &neighbors, &neighbor_count);
      //            arts_printf("Neighbors: %lu %lu\n", i, neighbor_count);
      local_count += process_vertex(graph, i, neighbors, neighbor_count,
                                    &visit_mask, &proc_local, &proc_remote);
    }
    atomic_fetch_add_explicit(&local_triangle_count, local_count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&local, proc_local, memory_order_relaxed);
    atomic_fetch_add_explicit(&remote, proc_remote, memory_order_relaxed);
  }
  //    arts_printf("%lu : %lu\n", start, local_count);
}

void init_node(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int node_id = arts_get_current_rank();

  int rc = arts_csr_load_from_args(distribution, saved_argc, saved_argv);

  dist_start = arts_block_dist_partition_start(node_id, distribution);
  dist_end = dist_start + arts_block_dist_block_size(node_id, distribution);
  block_size = (arts_block_dist_partition_end(node_id, distribution) -
                arts_block_dist_partition_start(node_id, distribution)) /
               (arts_get_total_workers() * over_sub);
  if (!block_size) {
    block_size = 1;
  }

  if (rc != 0) {
    arts_printf("Failed to load the graph on rank %u\n", node_id);
    arts_shutdown();
  }
}

void start_work(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int node_id = arts_get_current_rank();
  arts_guid_t partition =
      arts_block_dist_guid_for_partition(distribution, node_id);

  uint64_t args[2];
  for (arts_vertex_t i = dist_start; i < dist_end; i += block_size) {
    args[0] = i;
    args[1] = (i + block_size < dist_end) ? i + block_size : dist_end;
    arts_guid_t guid = arts_edt_create(visit_vertex, 2, args, 1,
                                       &(arts_edt_hint_t){.rank = node_id});
    arts_add_dependence(partition, guid, 0, DB_MODE_RO);
  }
}

/* main_edt after every loader has finished, on rank 0. */
void main_continue(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  // Master creates reduce EDTs
  timer = arts_get_time_stamp();
  arts_edt_create(start_reduce, 0, NULL, 1,
                  &(arts_edt_hint_t){.guid = start_reduce_guid});
  arts_edt_create(final_reduce, 0, NULL, arts_get_total_ranks(),
                  &(arts_edt_hint_t){.guid = final_reduce_guid});

  // Start work on every node
  arts_guid_t work_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int i = 0; i < arts_get_total_ranks(); i++) {
    arts_edt_create(start_work, 0, NULL, 0,
                    &(arts_edt_hint_t){.rank = i, .finish_event = work_done});
  }
  arts_add_dependence(work_done, start_reduce_guid, 0, DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  if (distribution == NULL) {
    arts_printf("Usage: %s --file <graph> --num-vertices <n> --num-edges <m> "
                "[--flip] [--keep-self-loops] [--csr-format]\n",
                ((char **)paramv[1])[0]);
    arts_shutdown();
    return;
  }
  if (arts_get_total_ranks() > 64) {
    arts_printf("triangle_count supports at most 64 ranks, not %u\n",
                arts_get_total_ranks());
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
  start_reduce_guid = arts_guid_reserve(ARTS_GUID_EDT, 0);
  final_reduce_guid = arts_guid_reserve(ARTS_GUID_EDT, 0);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
