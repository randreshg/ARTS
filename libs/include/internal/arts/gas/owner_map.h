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

/// @file owner_map.h
/// @brief Mechanical evaluator for committed distributed-DB owner-map facts.
///
/// Computes the single owner rank of a distributed datablock block from the
/// owner-map facts the CARTS compiler commits on `arts.db_alloc`
/// (DbOwnerMapKind + owner_map_dims). It is a pure validator/evaluator: it does
/// NOT mint GUIDs, create DBs, or route live traffic. Live ownership is still
/// carried by the GUID rank field, which the compiler bakes in at lowering time
/// (arts_guid_create_for_rank / ARTS_GUID_GET_RANK). This evaluator reproduces
/// that owner arithmetic so the runtime can interpret the committed facts.
///
/// Invariants (required for the result to agree with the live GUID rank):
///   - Kind codes equal the ARTS dialect DbOwnerMapKind enum (0..3) verbatim.
///   - Sizes are DB block-grid extents (datablock counts per dim), not element
///     extents; coordinates index that block grid in row-major (C) order.
///   - owner_block_shape is intentionally absent: the committed route formula
///     does not consume it.
///   - The result is a pure function of the inputs (no globals), so every node
///     derives the same single owner for a block (CDAG canonical-owner rule).
///   - Mechanical only: missing/malformed/inconsistent facts fail closed, and
///     the not-yet-committed kinds owner_dim_grid and explicit_rank_table are
///     rejected until the dialect commits their parameters and formula.

#ifndef ARTS_GAS_OWNER_MAP_H
#define ARTS_GAS_OWNER_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Maximum DB rank the evaluator accepts (bounds the un-ranking work buffer).
#define ARTS_OWNER_MAP_MAX_RANK 8

/// Owner-map kind. Integer values MUST match the ARTS dialect DbOwnerMapKind.
typedef enum arts_owner_map_kind {
  ARTS_OWNER_MAP_LINEAR_MOD_NODES = 0,
  ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS = 1,
  ARTS_OWNER_MAP_OWNER_DIM_GRID = 2,
  ARTS_OWNER_MAP_EXPLICIT_RANK_TABLE = 3,
} arts_owner_map_kind_t;

/// Evaluation status. On ARTS_OWNER_MAP_OK the owner rank is written; every
/// other value is a fail-closed rejection that leaves the out parameter
/// untouched.
typedef enum arts_owner_map_status {
  ARTS_OWNER_MAP_OK = 0,
  ARTS_OWNER_MAP_ERR_NULL_ARG,         ///< a required pointer argument was NULL
  ARTS_OWNER_MAP_ERR_RANK,             ///< ndims == 0 or > ARTS_OWNER_MAP_MAX_RANK
  ARTS_OWNER_MAP_ERR_NODE_COUNT,       ///< total_nodes == 0 or beyond the supported node domain
  ARTS_OWNER_MAP_ERR_DB_SIZE,          ///< a block-grid extent was <= 0
  ARTS_OWNER_MAP_ERR_SIZE_OVERFLOW,    ///< block-grid product exceeds the supported index domain
  ARTS_OWNER_MAP_ERR_COORD_RANGE,      ///< a block coordinate was outside [0, extent)
  ARTS_OWNER_MAP_ERR_LINEAR_RANGE,     ///< linear index outside [0, block count)
  ARTS_OWNER_MAP_ERR_OWNER_DIMS,       ///< owner_dim_contiguous: empty or out-of-range owner dim
  ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND, ///< owner_dim_grid / explicit_rank_table: no committed params/formula
  ARTS_OWNER_MAP_ERR_UNKNOWN_KIND,     ///< kind code outside [0, 3]
} arts_owner_map_status_t;

/// Owner rank of the block at `block_coords` in the `ndims`-D block grid
/// `db_block_grid`. For ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, `owner_dims`
/// (length `owner_dim_count`, in owner-slot order) selects the participating
/// block-grid dims; ARTS_OWNER_MAP_LINEAR_MOD_NODES ignores `owner_dims` (all
/// dims participate). `total_nodes` is the divisor/modulus. On OK the owner is
/// in [0, total_nodes).
arts_owner_map_status_t arts_owner_map_owner_for_coords(
    arts_owner_map_kind_t kind, const int64_t *db_block_grid,
    const int64_t *block_coords, size_t ndims, const int64_t *owner_dims,
    size_t owner_dim_count, unsigned int total_nodes,
    unsigned int *out_owner_rank);

/// As arts_owner_map_owner_for_coords, but addressing the block by its
/// row-major linear index in [0, product(db_block_grid)).
arts_owner_map_status_t arts_owner_map_owner_for_linear_index(
    arts_owner_map_kind_t kind, const int64_t *db_block_grid, size_t ndims,
    int64_t block_linear_index, const int64_t *owner_dims,
    size_t owner_dim_count, unsigned int total_nodes,
    unsigned int *out_owner_rank);

/// Human-readable name for a status, for fail-closed diagnostics/evidence.
const char *arts_owner_map_status_str(arts_owner_map_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_GAS_OWNER_MAP_H */
