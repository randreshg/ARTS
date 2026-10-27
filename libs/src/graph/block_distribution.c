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
#include "arts/graph.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "arts.h"
#include "arts/gas/guid.h"
#include "arts/system/print.h"

arts_block_dist_t arts_block_dist_create(arts_graph_sz_t n, arts_graph_sz_t m,
                                         unsigned int num_blocks,
                                         arts_guid_kind_t kind) {
  if (kind <= ARTS_GUID_RESERVED || kind >= ARTS_GUID_LAST) {
    ARTS_ERROR("arts_block_dist_create: %u is not a GUID kind",
               (unsigned int)kind);
  }
  unsigned int nrank = arts_get_total_ranks();
  /* Partition p is member k(p) * nrank + h(p) of one round-robin range, so
   * the range holds ceil(num_blocks / nrank) members per home. */
  uint64_t per_home = num_blocks / nrank + (num_blocks % nrank != 0);
  uint64_t size = per_home * nrank;
  if (size > UINT_MAX) {
    ARTS_ERROR("a distribution of %u blocks over %u ranks needs %lu names; "
               "one range holds at most %u",
               num_blocks, nrank, (unsigned long)size, UINT_MAX);
  }
  arts_block_dist_t dist;
  dist.num_vertices = n;
  dist.num_edges = m;
  dist.num_blocks = num_blocks;
  dist.block_sz =
      (num_blocks > 0) ? (n / num_blocks + (n % num_blocks != 0)) : 0;
  dist.range = (num_blocks > 0)
                   ? arts_guid_reserve_range(kind, (unsigned int)size,
                                             ARTS_HINT_ROUND_ROBIN)
                   : NULL_GUID;
  return dist;
}

int arts_block_dist_init_from_args(arts_block_dist_t *out, int argc,
                                   const char *const *argv) {
  uint64_t n = 0;
  uint64_t m = 0;
  for (int i = 0; i < argc; ++i) {
    // A value-bearing flag consumes the following token; only read it when one
    // exists so a flag in the last position cannot read past the argument
    // array.
    if (strcmp("--num-vertices", argv[i]) == 0 && i + 1 < argc) {
      n = (uint64_t)strtoull(argv[i + 1], NULL, 10);
    }
    if (strcmp("--num-edges", argv[i]) == 0 && i + 1 < argc) {
      m = (uint64_t)strtoull(argv[i + 1], NULL, 10);
    }
  }

  if (n && m) {
    *out = arts_block_dist_create(n, m, arts_get_total_ranks(), ARTS_GUID_DB);
    return 0;
  }
  ARTS_INFO("Must set --num-vertices and --num-edges");
  return -1;
}

void arts_block_dist_to_params(const arts_block_dist_t *dist,
                               uint64_t *paramv) {
  paramv[0] = dist->num_vertices;
  paramv[1] = dist->num_edges;
  paramv[2] = dist->block_sz;
  paramv[3] = dist->num_blocks;
  paramv[4] = dist->range;
}

arts_block_dist_t arts_block_dist_from_params(const uint64_t *paramv) {
  arts_block_dist_t dist;
  dist.num_vertices = paramv[0];
  dist.num_edges = paramv[1];
  dist.block_sz = paramv[2];
  dist.num_blocks = (unsigned int)paramv[3];
  dist.range = (arts_guid_t)paramv[4];
  if (paramv[3] > UINT_MAX ||
      (dist.num_blocks == 0) != (dist.range == NULL_GUID) ||
      (dist.num_blocks > 0 &&
       ARTS_GUID_GET_RANK(dist.range) != ARTS_DISTRIBUTED_RANK)) {
    ARTS_ERROR("arts_block_dist_from_params: the words are not a "
               "distribution (%lu blocks, range %lu)",
               (unsigned long)paramv[3], (unsigned long)dist.range);
  }
  return dist;
}

arts_graph_sz_t
arts_block_dist_block_size(unsigned int index,
                           const arts_block_dist_t *const dist) {
  // A block covers the half-open vertex range [index*block_sz, num_vertices)
  // capped at block_sz.  Computing the size as a difference (rather than a
  // special-case for the last block) keeps the partition exact and never
  // underflows the unsigned size: a block whose start is at/after the vertex
  // count is simply empty.
  arts_graph_sz_t start = (arts_graph_sz_t)index * dist->block_sz;
  if (start >= dist->num_vertices) {
    return 0;
  }
  arts_graph_sz_t remaining = dist->num_vertices - start;
  return (remaining < dist->block_sz) ? remaining : dist->block_sz;
}

unsigned int arts_block_dist_get_owner(arts_vertex_t v,
                                       const arts_block_dist_t *const dist) {
  // An empty distribution (no blocks => zero block size) has no owner for any
  // vertex; guard the division so a degenerate dist cannot trigger a
  // divide-by-zero.
  if (dist->block_sz == 0) {
    return 0;
  }
  return (unsigned int)(v / dist->block_sz);
}

arts_vertex_t
arts_block_dist_partition_start(arts_partition_t index,
                                const arts_block_dist_t *const dist) {
  return (arts_vertex_t)((dist->block_sz) * index);
}

arts_vertex_t
arts_block_dist_partition_end(arts_partition_t index,
                              const arts_block_dist_t *const dist) {
  // Last vertex index in this block, derived from the block's actual size so an
  // empty block (size 0, e.g. an empty graph) reports its start rather than
  // underflowing the unsigned index below zero.
  arts_vertex_t start = arts_block_dist_partition_start(index, dist);
  arts_graph_sz_t size = arts_block_dist_block_size(index, dist);
  return (size == 0) ? start : (arts_vertex_t)(start + size - 1);
}

arts_local_index_t
arts_block_dist_get_local_index(arts_vertex_t v,
                                const arts_block_dist_t *const dist) {
  unsigned int n = arts_block_dist_get_owner(v, dist);
  arts_vertex_t base = arts_block_dist_partition_start(n, dist);
  assert(base <= v);
  return (v - base);
}

arts_guid_t
arts_block_dist_guid_for_vertex(arts_vertex_t v,
                                const arts_block_dist_t *const dist) {
  return arts_block_dist_guid_for_partition(dist,
                                            arts_block_dist_get_owner(v, dist));
}

arts_guid_t
arts_block_dist_guid_for_partition(const arts_block_dist_t *const dist,
                                   arts_partition_t index) {
  /* A past-end index would derive a member the range does not hold, which
   * can alias a later mint of the reserving rank. */
  if (index >= dist->num_blocks) {
    ARTS_ERROR("arts_block_dist_guid_for_partition: partition %u of a "
               "distribution of %u",
               index, dist->num_blocks);
  }
  /* Rank h holds num_blocks / nrank consecutive partitions, plus one for the
   * first num_blocks % nrank ranks; its k-th is member k * nrank + h. */
  unsigned int nrank = arts_get_total_ranks();
  unsigned int per = dist->num_blocks / nrank;
  unsigned int mod = dist->num_blocks % nrank;
  unsigned int split = mod * (per + 1);
  unsigned int home;
  unsigned int ordinal;
  if (index < split) {
    home = index / (per + 1);
    ordinal = index % (per + 1);
  } else {
    home = mod + (index - split) / per;
    ordinal = (index - split) % per;
  }
  return arts_guid_from_index(dist->range, (ordinal * nrank) + home);
}
