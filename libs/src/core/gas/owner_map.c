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

/// @file owner_map.c
/// @brief Mechanical owner-rank arithmetic for committed owner-map facts.
///
/// The route formulas reproduce the compiler's committed arithmetic
/// (DistributedDbPlacementUtils.h createDbOwnerRoute*): row-major (C-order)
/// block linearization, linear_mod_nodes = linear % total_nodes, and
/// owner_dim_contiguous = (owner_linear * total_nodes) / owner_space over the
/// owner-dim sub-grid. Block grids AND node counts are capped to the signed
/// 32-bit index domain so the int64 arithmetic here matches the compiler's i32
/// lowering exactly within the accepted range and fails closed beyond it.

#include "arts/gas/owner_map.h"

/// Largest block count the evaluator accepts. The compiler lowers the block
/// linear index through i32 arithmetic, so staying within this domain keeps the
/// int64 result bit-identical and rejects (rather than silently truncates)
/// anything the live path could not represent.
#define ARTS_OWNER_MAP_MAX_BLOCKS INT32_MAX

const char *arts_owner_map_status_str(arts_owner_map_status_t status) {
  switch (status) {
  case ARTS_OWNER_MAP_OK:
    return "ok";
  case ARTS_OWNER_MAP_ERR_NULL_ARG:
    return "null argument";
  case ARTS_OWNER_MAP_ERR_RANK:
    return "unsupported DB rank";
  case ARTS_OWNER_MAP_ERR_NODE_COUNT:
    return "zero node count";
  case ARTS_OWNER_MAP_ERR_DB_SIZE:
    return "non-positive block-grid extent";
  case ARTS_OWNER_MAP_ERR_SIZE_OVERFLOW:
    return "block-grid product out of range";
  case ARTS_OWNER_MAP_ERR_COORD_RANGE:
    return "block coordinate out of range";
  case ARTS_OWNER_MAP_ERR_LINEAR_RANGE:
    return "block linear index out of range";
  case ARTS_OWNER_MAP_ERR_OWNER_DIMS:
    return "invalid owner dims";
  case ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND:
    return "owner-map kind not yet committed";
  case ARTS_OWNER_MAP_ERR_UNKNOWN_KIND:
    return "unknown owner-map kind";
  }
  return "unknown status";
}

/// Validate the block grid and (when requested) return its block count. Fails
/// closed on non-positive extents and on products beyond the supported index
/// domain, so callers can linearize without overflow.
static arts_owner_map_status_t grid_block_count(const int64_t *grid,
                                                size_t ndims,
                                                int64_t *out_count) {
  int64_t count = 1;
  for (size_t i = 0; i < ndims; ++i) {
    int64_t extent = grid[i];
    if (extent <= 0)
      return ARTS_OWNER_MAP_ERR_DB_SIZE;
    if (extent > ARTS_OWNER_MAP_MAX_BLOCKS ||
        count > ARTS_OWNER_MAP_MAX_BLOCKS / extent)
      return ARTS_OWNER_MAP_ERR_SIZE_OVERFLOW;
    count *= extent;
  }
  if (out_count)
    *out_count = count;
  return ARTS_OWNER_MAP_OK;
}

/// Reject the kinds with no committed parameters/formula and any out-of-range
/// kind code before any computation, mirroring the compiler's fail-closed
/// contract (UnsupportedOwnerMapKind) so the runtime never guesses an owner.
static arts_owner_map_status_t check_supported_kind(arts_owner_map_kind_t kind) {
  switch (kind) {
  case ARTS_OWNER_MAP_LINEAR_MOD_NODES:
  case ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS:
    return ARTS_OWNER_MAP_OK;
  case ARTS_OWNER_MAP_OWNER_DIM_GRID:
  case ARTS_OWNER_MAP_EXPLICIT_RANK_TABLE:
    return ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND;
  }
  return ARTS_OWNER_MAP_ERR_UNKNOWN_KIND;
}

arts_owner_map_status_t arts_owner_map_owner_for_coords(
    arts_owner_map_kind_t kind, const int64_t *db_block_grid,
    const int64_t *block_coords, size_t ndims, const int64_t *owner_dims,
    size_t owner_dim_count, unsigned int total_nodes,
    unsigned int *out_owner_rank) {
  if (!out_owner_rank || !db_block_grid || !block_coords)
    return ARTS_OWNER_MAP_ERR_NULL_ARG;
  if (ndims == 0 || ndims > ARTS_OWNER_MAP_MAX_RANK)
    return ARTS_OWNER_MAP_ERR_RANK;
  /// Cap node count to the same i32 index domain as the block grid: the
  /// compiler casts total_nodes through a signed i32->index cast, so node
  /// counts above INT32_MAX would diverge from the live route. Fail closed
  /// rather than return a value the lowered path could not reproduce.
  if (total_nodes == 0 || total_nodes > (unsigned int)INT32_MAX)
    return ARTS_OWNER_MAP_ERR_NODE_COUNT;

  arts_owner_map_status_t st = check_supported_kind(kind);
  if (st != ARTS_OWNER_MAP_OK)
    return st;

  st = grid_block_count(db_block_grid, ndims, NULL);
  if (st != ARTS_OWNER_MAP_OK)
    return st;

  for (size_t i = 0; i < ndims; ++i)
    if (block_coords[i] < 0 || block_coords[i] >= db_block_grid[i])
      return ARTS_OWNER_MAP_ERR_COORD_RANGE;

  if (kind == ARTS_OWNER_MAP_LINEAR_MOD_NODES) {
    /// Cyclic: full row-major linear index modulo node count. owner_dims are
    /// not consulted (every dim participates), matching the committed formula.
    int64_t linear = block_coords[0];
    for (size_t i = 1; i < ndims; ++i)
      linear = linear * db_block_grid[i] + block_coords[i];
    *out_owner_rank = (unsigned int)(linear % (int64_t)total_nodes);
    return ARTS_OWNER_MAP_OK;
  }

  /// owner_dim_contiguous: contiguous block-of-ranks over the owner sub-grid.
  if (owner_dim_count == 0)
    return ARTS_OWNER_MAP_ERR_OWNER_DIMS;
  if (!owner_dims)
    return ARTS_OWNER_MAP_ERR_NULL_ARG;
  /// Owner dims must be in range AND distinct: a repeated dim is geometrically
  /// inconsistent and would re-multiply an extent, letting owner_space escape
  /// the grid cap and overflow. ndims <= ARTS_OWNER_MAP_MAX_RANK, so a bitset
  /// over the dims suffices.
  uint64_t owner_dim_seen = 0;
  for (size_t k = 0; k < owner_dim_count; ++k) {
    int64_t dim = owner_dims[k];
    if (dim < 0 || (size_t)dim >= ndims)
      return ARTS_OWNER_MAP_ERR_OWNER_DIMS;
    if (owner_dim_seen & ((uint64_t)1 << dim))
      return ARTS_OWNER_MAP_ERR_OWNER_DIMS;
    owner_dim_seen |= (uint64_t)1 << dim;
  }

  int64_t owner_linear = 0;
  int64_t owner_space = 1;
  for (size_t k = 0; k < owner_dim_count; ++k) {
    int64_t dim = owner_dims[k];
    owner_linear = owner_linear * db_block_grid[dim] + block_coords[dim];
    owner_space *= db_block_grid[dim];
  }
  /// Distinct owner dims keep owner_space within the grid cap and the node-count
  /// cap keeps owner_linear * total_nodes within int64 range; owner_space >= 1
  /// and owner_linear < owner_space, so the route is in [0, total_nodes).
  *out_owner_rank =
      (unsigned int)((owner_linear * (int64_t)total_nodes) / owner_space);
  return ARTS_OWNER_MAP_OK;
}

arts_owner_map_status_t arts_owner_map_owner_for_linear_index(
    arts_owner_map_kind_t kind, const int64_t *db_block_grid, size_t ndims,
    int64_t block_linear_index, const int64_t *owner_dims,
    size_t owner_dim_count, unsigned int total_nodes,
    unsigned int *out_owner_rank) {
  if (!out_owner_rank || !db_block_grid)
    return ARTS_OWNER_MAP_ERR_NULL_ARG;
  if (ndims == 0 || ndims > ARTS_OWNER_MAP_MAX_RANK)
    return ARTS_OWNER_MAP_ERR_RANK;
  /// Cap node count to the same i32 index domain as the block grid: the
  /// compiler casts total_nodes through a signed i32->index cast, so node
  /// counts above INT32_MAX would diverge from the live route. Fail closed
  /// rather than return a value the lowered path could not reproduce.
  if (total_nodes == 0 || total_nodes > (unsigned int)INT32_MAX)
    return ARTS_OWNER_MAP_ERR_NODE_COUNT;

  arts_owner_map_status_t st = check_supported_kind(kind);
  if (st != ARTS_OWNER_MAP_OK)
    return st;

  int64_t block_count;
  st = grid_block_count(db_block_grid, ndims, &block_count);
  if (st != ARTS_OWNER_MAP_OK)
    return st;

  if (block_linear_index < 0 || block_linear_index >= block_count)
    return ARTS_OWNER_MAP_ERR_LINEAR_RANGE;

  if (kind == ARTS_OWNER_MAP_LINEAR_MOD_NODES) {
    /// Cyclic shortcut: the linear index is already the value mod node count.
    *out_owner_rank = (unsigned int)(block_linear_index % (int64_t)total_nodes);
    return ARTS_OWNER_MAP_OK;
  }

  /// owner_dim_contiguous: un-rank to row-major coords, then evaluate. Mirrors
  /// the compiler delegating createDbOwnerRouteForLinearIndex -> ForCoords.
  int64_t coords[ARTS_OWNER_MAP_MAX_RANK];
  int64_t remaining = block_linear_index;
  for (size_t i = 0; i < ndims; ++i) {
    if (i + 1 == ndims) {
      coords[i] = remaining;
      break;
    }
    int64_t stride = 1;
    for (size_t j = i + 1; j < ndims; ++j)
      stride *= db_block_grid[j];
    coords[i] = remaining / stride;
    remaining %= stride;
  }
  return arts_owner_map_owner_for_coords(kind, db_block_grid, coords, ndims,
                                         owner_dims, owner_dim_count,
                                         total_nodes, out_owner_rank);
}
