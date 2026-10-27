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

#ifndef ARTS_GRAPH_H
#define ARTS_GRAPH_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts.h"
#include <stdbool.h>
#include <stdint.h>

/* === Graph primitive types === */

typedef uint64_t arts_vertex_t;
typedef uint64_t arts_graph_sz_t;
typedef unsigned int arts_partition_t;
typedef uint32_t arts_edge_data_t;
typedef uint64_t arts_local_index_t;

typedef struct {
  arts_vertex_t source;
  arts_vertex_t target;
  arts_edge_data_t data;
} arts_edge_t;

/* === Edge vector === */

#define ARTS_EDGE_VEC_SZ 10000

typedef struct {
  arts_edge_t *edge_array;
  arts_graph_sz_t used;
  arts_graph_sz_t size;
} arts_edge_vector_t;

void arts_edge_vector_init(arts_edge_vector_t *v, arts_graph_sz_t initial_size);
void arts_edge_vector_push_back(arts_edge_vector_t *v, arts_vertex_t s,
                                arts_vertex_t t, arts_edge_data_t d);
void arts_edge_vector_free(arts_edge_vector_t *v);
void arts_edge_vector_sort_by_source(arts_edge_vector_t *v);
void arts_edge_vector_sort_by_source_and_target(arts_edge_vector_t *v);

/* === Block distribution ===
 *
 * A distribution splits the vertex range [0, num_vertices) into num_blocks
 * partitions of at most block_sz consecutive vertices and names them by one
 * round-robin range of GUIDs of the given kind (ARTS_GUID_DB for a CSR graph):
 * rank i owns num_blocks / N consecutive partitions, plus one for the first
 * num_blocks % N ranks, and a partition's GUID is homed on its owner.
 * A distribution is a value.  Create it once, in one EDT, and pass the value
 * on in parameters (arts_block_dist_to_params / arts_block_dist_from_params,
 * ARTS_BLOCK_DIST_PARAMC words): every rank then derives the same partition
 * GUIDs.  Two creates reserve two different ranges. */

typedef struct {
  arts_graph_sz_t num_vertices;
  arts_graph_sz_t num_edges;
  arts_graph_sz_t block_sz;
  unsigned int num_blocks;
  /* The round-robin range naming the partitions; NULL_GUID iff
   * num_blocks == 0. */
  arts_guid_t range;
} arts_block_dist_t;

/* The parameter words a distribution takes. */
#define ARTS_BLOCK_DIST_PARAMC 5u

/* Makes a distribution and reserves its partitions' names (one round-robin
 * range of N * ceil(num_blocks / N) GUIDs of `kind`).  Call once, in one EDT,
 * and pass the value on: a value is made only here or by
 * arts_block_dist_from_params. */
arts_block_dist_t arts_block_dist_create(arts_graph_sz_t n, arts_graph_sz_t m,
                                         unsigned int num_blocks,
                                         arts_guid_kind_t kind);
/* --num-vertices / --num-edges, one ARTS_GUID_DB partition per rank; calls
 * arts_block_dist_create.  Returns 0, or -1 (nothing reserved) when a count is
 * missing. */
int arts_block_dist_init_from_args(arts_block_dist_t *out, int argc,
                                   const char *const *argv);
void arts_block_dist_to_params(const arts_block_dist_t *dist, uint64_t *paramv);
/* Reads exactly ARTS_BLOCK_DIST_PARAMC words at paramv. */
arts_block_dist_t arts_block_dist_from_params(const uint64_t *paramv);

arts_graph_sz_t arts_block_dist_block_size(arts_partition_t index,
                                           const arts_block_dist_t *dist);

arts_partition_t arts_block_dist_get_owner(arts_vertex_t v,
                                           const arts_block_dist_t *dist);
arts_vertex_t arts_block_dist_partition_start(arts_partition_t index,
                                              const arts_block_dist_t *dist);
arts_vertex_t arts_block_dist_partition_end(arts_partition_t index,
                                            const arts_block_dist_t *dist);
arts_local_index_t
arts_block_dist_get_local_index(arts_vertex_t v, const arts_block_dist_t *dist);

/* A partition index at or past num_blocks is fatal. */
arts_guid_t arts_block_dist_guid_for_vertex(arts_vertex_t v,
                                            const arts_block_dist_t *dist);
arts_guid_t arts_block_dist_guid_for_partition(const arts_block_dist_t *dist,
                                               arts_partition_t index);

/* === CSR graph ===
 *
 * Each partition is a coherent ARTS_DB block created under its distribution
 * GUID on its home rank.  The block is self-contained: an arts_csr_graph_t
 * header followed by num_local_vertices + 1 row offsets and num_local_edges
 * target vertices; a loader sorts each row's targets in ascending order.
 *
 * Contract:
 *   - Load from an EDT on each rank (arts_csr_load_*): the loader creates the
 *     partitions homed on its rank, and that EDT holds each RW until it ends.
 *   - Order every reader after every loader, e.g. loaders as members of one
 *     finish scope and readers created by its continuation.
 *   - Read a partition only through an acquisition: a dependence slot on its
 *     GUID, normally RO, whose depv[i].ptr is the arts_csr_graph_t that
 *     arts_csr_get_neighbors and arts_csr_get_local_index take.
 *   - Destroy a partition (arts_db_destroy on its GUID, or arts_csr_free on
 *     an acquired pointer) only after its last reader.
 *   - Load a distribution once: a second load while its partitions live
 *     creates live GUIDs again, two creates of one GUID with no destroy
 *     between them, which is undefined.
 *
 * The loaders return 0, or -1 when the file cannot be read or does not parse
 * (a missing or inconsistent adjacency header, a line that is not an edge, an
 * endpoint at or above num_vertices); on -1 the calling rank creates no
 * partition.  Edge list (arts_csr_load_no_weight): one "source target" pair
 * per line, further fields ignored; blank and '#' lines are skipped; '%' lines
 * before the first pair mark 1-based ids, and after a "%%MatrixMarket" banner
 * the first data line is the size line; later '%' lines are comments.
 * Adjacency (arts_csr_load_no_weight_csr): a "<vertices> <edges>" header,
 * then line i lists the 1-based targets of vertex i; '%' and '#' lines and 0
 * tokens are skipped.  flip adds the reverse
 * of every edge; ignore_self_loops drops an edge whose ends are equal.
 * arts_csr_load_from_args takes --file <path>, --flip, --keep-self-loops
 * (self loops are dropped otherwise) and --csr-format (the adjacency
 * format). */

#define ARTS_GRAPH_MAXCHAR (1024 * 1024)

typedef struct {
  arts_guid_t partGuid;
  arts_graph_sz_t num_local_vertices;
  arts_graph_sz_t num_local_edges;
  arts_graph_sz_t block_sz;
  arts_partition_t index;
  unsigned int num_blocks;
} arts_csr_graph_t;

/* Creates partition part_index under block_guid from edges whose sources
 * all lie in it, sorting them by source unless sorted_by_src, and returns the
 * creator's writable image; NULL when block_guid is homed on another rank. */
arts_csr_graph_t *arts_csr_init(arts_partition_t part_index,
                                arts_graph_sz_t localv, arts_graph_sz_t locale,
                                const arts_block_dist_t *dist,
                                arts_edge_vector_t *edges, bool sorted_by_src,
                                arts_guid_t block_guid);
int arts_csr_load_no_weight(const char *file_path,
                            const arts_block_dist_t *dist, bool flip,
                            bool ignore_self_loops);
int arts_csr_load_no_weight_csr(const char *file_path,
                                const arts_block_dist_t *dist, bool flip,
                                bool ignore_self_loops);
int arts_csr_load_from_args(const arts_block_dist_t *dist, int argc,
                            const char *const *argv);
void arts_csr_free(arts_csr_graph_t *csr);
void arts_csr_print(arts_csr_graph_t *csr);
void arts_csr_get_neighbors(arts_csr_graph_t *csr, arts_vertex_t v,
                            arts_vertex_t **out,
                            arts_graph_sz_t *neighborcount);
arts_local_index_t arts_csr_get_local_index(arts_vertex_t v,
                                            const arts_csr_graph_t *part);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_GRAPH_H */
